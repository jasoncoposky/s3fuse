#include <string.h>

#include "object.hh"
#include "request.hh"

using namespace std;

using namespace s3;

namespace
{
  const int     BLOCK_SIZE                    = 512;
  const string  AMZ_META_PREFIX               = "x-amz-meta-";
  const string  AMZ_META_PREFIX_RESERVED      = "s3fuse-";
  const char   *AMZ_META_PREFIX_CSTR          = AMZ_META_PREFIX.c_str();
  const char   *AMZ_META_PREFIX_RESERVED_CSTR = AMZ_META_PREFIX_RESERVED.c_str();
  const size_t  AMZ_META_PREFIX_LEN           = AMZ_META_PREFIX.size();
  const size_t  AMZ_META_PREFIX_RESERVED_LEN  = AMZ_META_PREFIX_RESERVED.size();
  const char   *SYMLINK_CONTENT_TYPE          = "text/symlink";

  int    g_default_uid                        = 1000;
  int    g_default_gid                        = 1000;
  int    g_default_mode                       = 0755;
  int    g_expiry_in_s                        = 3 * 60; // 3 minutes
  string g_default_content_type               = "binary/octet-stream";
  string g_bucket                             = "test-0";
  string g_bucket_url                         = "/" + util::url_encode(g_bucket);

  mode_t get_mode_by_type(object_type type)
  {
    if (type == OT_FILE)
      return S_IFREG;

    if (type == OT_DIRECTORY)
      return S_IFDIR;

    if (type == OT_SYMLINK)
      return S_IFLNK;

    return 0;
  }
}

const string & object::get_bucket_url()
{
  return g_bucket_url;
}

string object::build_url(const string &path, object_type type)
{
  return g_bucket_url + "/" + util::url_encode(path) + ((type == OT_DIRECTORY) ? "/" : "");
}

void object::set_defaults(object_type type)
{
  memset(&_stat, 0, sizeof(_stat));

  _stat.st_uid = g_default_uid;
  _stat.st_gid = g_default_gid;
  _stat.st_mode = g_default_mode | get_mode_by_type(type);
  _stat.st_nlink  = 1; // laziness (see FUSE FAQ re. find)
  _stat.st_mtime = time(NULL);

  _type = type;
  _content_type = ((type == OT_SYMLINK) ? string(SYMLINK_CONTENT_TYPE) : g_default_content_type);
  _etag.clear();
  _mtime_etag.clear();
  _md5.clear();
  _md5_etag.clear();
  _expiry = time(NULL) + g_expiry_in_s;
  _metadata.clear();
  _url = build_url(_path, _type);
}

int object::set_metadata(const string &key, const string &value)
{
  if (strncmp(key.c_str(), AMZ_META_PREFIX_RESERVED_CSTR, AMZ_META_PREFIX_RESERVED_LEN) == 0)
    return -EINVAL;

  _metadata[key] = value;
  return 0;
}

void object::set_mode(mode_t mode)
{
  mode = mode & ~S_IFMT;

  if (mode == 0)
    mode = g_default_mode;

  _stat.st_mode = (_stat.st_mode & S_IFMT) | mode;
}

void object::request_init()
{
  memset(&_stat, 0, sizeof(_stat));

  _type = OT_INVALID;
  _content_type.clear();
  _etag.clear();
  _mtime_etag.clear();
  _md5.clear();
  _md5_etag.clear();
  _expiry = 0;
  _metadata.clear();
  _url.clear();
}

void object::request_process_header(const std::string &key, const std::string &value)
{
  long long_value = strtol(value.c_str(), NULL, 0);

  if (key == "Content-Type")
    _content_type = value;
  else if (key == "ETag")
    _etag = value;
  else if (key == "Content-Length")
    _stat.st_size = long_value;
  else if (key == "x-amz-meta-s3fuse-mode")
    _stat.st_mode = long_value & ~S_IFMT;
  else if (key == "x-amz-meta-s3fuse-uid")
    _stat.st_uid = long_value;
  else if (key == "x-amz-meta-s3fuse-gid")
    _stat.st_gid = long_value;
  else if (key == "x-amz-meta-s3fuse-mtime")
    _stat.st_mtime = long_value;
  else if (key == "x-amz-meta-s3fuse-mtime-etag")
    _mtime_etag = value;
  else if (key == "x-amz-meta-s3fuse-md5")
    _md5 = value;
  else if (key == "x-amz-meta-s3fuse-md5-etag")
    _md5_etag = value;
  else if (
    strncmp(key.c_str(), AMZ_META_PREFIX_CSTR, AMZ_META_PREFIX_LEN) == 0 &&
    strncmp(key.c_str() + AMZ_META_PREFIX_LEN, AMZ_META_PREFIX_RESERVED_CSTR, AMZ_META_PREFIX_RESERVED_LEN) != 0
  )
    _metadata[key.substr(AMZ_META_PREFIX_LEN)] = value;
}

void object::request_process_response(request *req)
{
  const string &url = req->get_url();

  if (url.empty() || req->get_response_code() != 200)
    return;

  if (url[url.size() - 1] == '/')
    _type = OT_DIRECTORY;
  else if (_content_type == SYMLINK_CONTENT_TYPE)
    _type = OT_SYMLINK;
  else
    _type = OT_FILE;

  _url = build_url(_path, _type);

  _stat.st_mode   = (_stat.st_mode == 0) ? g_default_mode : _stat.st_mode;
  _stat.st_uid    = (_stat.st_uid  == 0) ? g_default_uid  : _stat.st_uid;
  _stat.st_gid    = (_stat.st_gid  == 0) ? g_default_gid  : _stat.st_gid;

  _stat.st_mode  |= get_mode_by_type(_type);
  _stat.st_nlink  = 1; // laziness (see FUSE FAQ re. find)

  // this workaround is for cases when the file was updated by someone else and the mtime header wasn't set
  if (_mtime_etag != _etag && req->get_last_modified() > _stat.st_mtime)
    _stat.st_mtime = req->get_last_modified();

  _mtime_etag = _etag;

  // this workaround is for multipart uploads, which don't get a valid md5 etag
  if (!util::is_valid_md5(_md5))
    _md5.clear();

  if ((_md5_etag != _etag || _md5.empty()) && util::is_valid_md5(_etag))
    _md5 = _etag;

  _md5_etag = _etag;

  if (_type == OT_FILE)
    _stat.st_blocks = (_stat.st_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

  // setting _expiry > 0 makes this object valid
  _expiry = time(NULL) + g_expiry_in_s;
}

void object::request_set_meta_headers(request *req)
{
  char buf[16];

  // do this first so that we overwrite any keys we care about (i.e., those that start with "x-amz-meta-s3fuse-")
  for (meta_map::const_iterator itor = _metadata.begin(); itor != _metadata.end(); ++itor)
    req->set_header(AMZ_META_PREFIX + itor->first, itor->second);

  snprintf(buf, 16, "%#o", _stat.st_mode & ~S_IFMT);
  req->set_header("x-amz-meta-s3fuse-mode", buf);

  snprintf(buf, 16, "%i", _stat.st_uid);
  req->set_header("x-amz-meta-s3fuse-uid", buf);

  snprintf(buf, 16, "%i", _stat.st_gid);
  req->set_header("x-amz-meta-s3fuse-gid", buf);

  snprintf(buf, 16, "%li", _stat.st_mtime);
  req->set_header("x-amz-meta-s3fuse-mtime", buf);

  req->set_header("x-amz-meta-s3fuse-mtime-etag", _mtime_etag);
  req->set_header("x-amz-meta-s3fuse-md5", _md5);
  req->set_header("x-amz-meta-s3fuse-md5-etag", _md5_etag);
  req->set_header("Content-Type", _content_type);
}
