#include "logging.hh"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include <stdexcept>

#include "config.hh"
#include "object.hh"
#include "openssl_locks.hh"
#include "request.hh"
#include "util.hh"

using namespace std;

using namespace s3;

#define TEST_OK(x) do { if ((x) != CURLE_OK) throw runtime_error("call to " #x " failed."); } while (0)

namespace
{
  const string AMZ_HEADER_PREFIX = "x-amz-";
}

request::request()
  : _current_run_time(0.0),
    _total_run_time(0.0),
    _run_count(0),
    _canceled(false),
    _timeout(0)
{
  _curl = curl_easy_init();

  if (!_curl)
    throw runtime_error("curl_easy_init() failed.");

  openssl_locks::init();

  // stuff that's set in the ctor shouldn't be modified elsewhere, since the call to init() won't reset it

  TEST_OK(curl_easy_setopt(_curl, CURLOPT_VERBOSE, config::get_verbose_requests()));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_NOPROGRESS, true));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, true));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_ERRORBUFFER, _curl_error));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_FILETIME, true));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_NOSIGNAL, true));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_HEADERFUNCTION, &request::process_header));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_HEADERDATA, this));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, &request::process_output));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_READFUNCTION, &request::process_input));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_READDATA, this));
}

request::~request()
{
  curl_easy_cleanup(_curl);

  if (_run_count > 0)
    S3_DEBUG(
      "request::~request", 
      "served %" PRIu64 " requests at an average of %.02f ms per request.\n", 
      _run_count,
      (_run_count && _total_run_time > 0.0) ? (_total_run_time / double(_run_count) * 1000.0) : 0.0);

  openssl_locks::release();
}

void request::init(http_method method)
{
  if (_canceled)
    throw runtime_error("cannot reuse a canceled request.");

  _curl_error[0] = '\0';
  _url.clear();
  _output_data.clear();
  _response_headers.clear();
  _response_code = 0;
  _last_modified = 0;
  _headers.clear();
  _target_object.reset();

  TEST_OK(curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, NULL));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_UPLOAD, false));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_NOBODY, false));
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_POST, false));

  if (method == HTTP_DELETE) {
    _method = "DELETE";
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "DELETE"));
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_NOBODY, true));

  } else if (method == HTTP_GET) {
    _method = "GET";

  } else if (method == HTTP_HEAD) {
    _method = "HEAD";
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_NOBODY, true));

  } else if (method == HTTP_POST) {
    _method = "POST";
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_POST, true));

  } else if (method == HTTP_PUT) {
    _method = "PUT";
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_UPLOAD, true));

  } else
    throw runtime_error("unsupported HTTP method.");

  // set these last because they depend on the value of _method
  set_input_fd();
  set_output_fd();
}

size_t request::process_header(char *data, size_t size, size_t items, void *context)
{
  request *req = static_cast<request *>(context);
  char *pos;

  size *= items;

  if (req->_canceled)
    return 0; // abort!

  if (data[size] != '\0')
    return size; // we choose not to handle the case where data isn't null-terminated

  pos = strchr(data, '\n');

  if (pos)
    *pos = '\0';

  // for some reason the ETag header (among others?) contains a carriage return
  pos = strchr(data, '\r'); 

  if (pos)
    *pos = '\0';

  pos = strchr(data, ':');

  if (!pos)
    return size; // no colon means it's not a header we care about

  *pos++ = '\0';

  if (*pos == ' ')
    pos++;

  if (req->_target_object)
    req->_target_object->request_process_header(data, pos);
  else
    req->_response_headers[data] = pos;

  return size;
}

size_t request::process_output(char *data, size_t size, size_t items, void *context)
{
  request *req = static_cast<request *>(context);

  // why even bother with "items"?
  size *= items;

  if (req->_canceled)
    return 0; // abort!

  if (req->_output_fd != -1) {
    ssize_t rc = pwrite(req->_output_fd, data, size, req->_output_offset);

    if (rc == -1)
      return 0;

    req->_output_offset += rc;
    return rc;

  } else {
    req->_output_data.append(data, size);

    return size;
  }
}

size_t request::process_input(char *data, size_t size, size_t items, void *context)
{
  request *req = static_cast<request *>(context);

  size *= items;

  if (req->_canceled)
    return 0; // abort!

  if (req->_input_fd != -1) {
    size_t remaining = (size > req->_input_size) ? req->_input_size : size;
    ssize_t rc = pread(req->_input_fd, data, remaining, req->_input_offset);

    if (rc == -1)
      return 0;

    req->_input_offset += rc;
    req->_input_size -= rc;
    return rc;

  } else {
    size_t remaining = req->_input_data.size() - req->_input_offset;

    if (size > remaining)
      size = remaining;

    if (remaining)
      req->_input_data.copy(data, size, req->_input_offset);

    req->_input_offset += size;
    return size;
  }
}

void request::set_url(const string &url, const string &query_string)
{
  string curl_url = config::get_url_prefix() + url;

  if (!query_string.empty()) {
    curl_url += (curl_url.find('?') == string::npos) ? "?" : "&";
    curl_url += query_string;
  }

  _url = url;
  TEST_OK(curl_easy_setopt(_curl, CURLOPT_URL, curl_url.c_str()));
}

void request::set_output_fd(int fd, off_t offset)
{
  if (fd == -1 && offset != 0)
    throw runtime_error("offset must be zero if an invalid fd is specified.");

  _output_fd = fd;
  _output_offset = offset;
}

void request::set_input_data(const string &s)
{
  _input_data = s;
  _input_fd = -1;
  _input_offset = 0;
  _input_size = 0;

  if (_method == "PUT")
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(s.size())));
  else if (_method == "POST")
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(s.size())));
  else if (!s.empty())
    throw runtime_error("can't set input data for non-POST/non-PUT request.");
}

void request::set_input_fd(int fd, size_t size, off_t offset)
{
  if (fd == -1 && (size != 0 || offset != 0))
    throw runtime_error("offset and size must be zero if an invalid fd is specified.");

  _input_data.clear();
  _input_fd = fd;
  _input_offset = offset;
  _input_size = size;

  if (_method == "PUT")
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size)));
  else if (_method == "POST")
    TEST_OK(curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size)));
  else if (size)
    throw runtime_error("can't set input fd for non-POST/non-PUT request.");
}

void request::set_meta_headers(const object::ptr &object)
{
  object->request_set_meta_headers(this); 
}

void request::build_request_time()
{
  time_t sys_time;
  tm gm_time;
  char time_str[128];

  time(&sys_time);
  gmtime_r(&sys_time, &gm_time);
  strftime(time_str, 128, "%a, %d %b %Y %H:%M:%S GMT", &gm_time);

  _headers["Date"] = time_str;
}

void request::build_signature()
{
  string sig = "AWS " + config::get_aws_key() + ":";
  string to_sign = _method + "\n" + _headers["Content-MD5"] + "\n" + _headers["Content-Type"] + "\n" + _headers["Date"] + "\n";

  for (header_map::const_iterator itor = _headers.begin(); itor != _headers.end(); ++itor)
    if (!itor->second.empty() && itor->first.substr(0, AMZ_HEADER_PREFIX.size()) == AMZ_HEADER_PREFIX)
      to_sign += itor->first + ":" + itor->second + "\n";

  to_sign += _url;
  _headers["Authorization"] = string("AWS ") + config::get_aws_key() + ":" + util::sign(config::get_aws_secret(), to_sign);
}

bool request::check_timeout()
{
  if (_timeout && time(NULL) > _timeout) {
    S3_DEBUG("request::check_timeout", "timed out.\n");

    _canceled = true;
    return true;
  }

  return false;
}

void request::run()
{
  int r;
  curl_slist *headers = NULL;
  double elapsed_time = util::get_current_time();

  // sanity
  if (_url.empty())
    throw runtime_error("call set_url() first!");

  if (_method.empty())
    throw runtime_error("call set_method() first!");

  if (_canceled)
    throw runtime_error("cannot reuse a canceled request.");

  _output_data.clear();
  _response_headers.clear();

  build_request_time();
  build_signature();

  for (header_map::const_iterator itor = _headers.begin(); itor != _headers.end(); ++itor)
    headers = curl_slist_append(headers, (itor->first + ": " + itor->second).c_str());

  TEST_OK(curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headers));

  if (_target_object)
    _target_object->request_init();

  _timeout = time(NULL) + config::get_request_timeout_in_s();
  r = curl_easy_perform(_curl);
  _timeout = 0; // reset this here so that subsequent calls to check_timeout() don't fail

  if (r != CURLE_OK)
    throw runtime_error(_curl_error);

  if (_canceled)
    throw runtime_error("request timed out.");

  TEST_OK(curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &_response_code));
  TEST_OK(curl_easy_getinfo(_curl, CURLINFO_FILETIME, &_last_modified));

  // TODO: add loop for timeouts and whatnot
  elapsed_time = util::get_current_time() - elapsed_time;

  if (_response_code >= 300 && _response_code != 404)
    S3_DEBUG("request::run", "request for [%s] failed with response: %s\n", _url.c_str(), _output_data.c_str());

  // don't save the time for the first request since it's likely to be disproportionately large
  if (_run_count > 0)
    _total_run_time += elapsed_time;

  // but save it in _current_run_time since it's compared to overall function time (i.e., it's relative)
  _current_run_time += elapsed_time;

  _run_count++;

  if (_target_object)
    _target_object->request_process_response(this);
}

