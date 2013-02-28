#include <errno.h>

#include <boost/lexical_cast.hpp>

#include "base/logger.h"
#include "base/request.h"
#include "fs/bucket_reader.h"
#include "services/service.h"

using boost::lexical_cast;
using std::string;

using s3::base::request;
using s3::base::xml;
using s3::fs::bucket_reader;
using s3::services::service;

namespace
{
  const char *IS_TRUNCATED_XPATH = "/ListBucketResult/IsTruncated";
  const char *         KEY_XPATH = "/ListBucketResult/Contents/Key";
  const char * NEXT_MARKER_XPATH = "/ListBucketResult/NextMarker";
  const char *      PREFIX_XPATH = "/ListBucketResult/CommonPrefixes/Prefix";
}

bucket_reader::bucket_reader(const string &prefix, bool group_common_prefixes, int max_keys)
  : _truncated(true),
    _prefix(prefix),
    _group_common_prefixes(group_common_prefixes),
    _max_keys(max_keys)
{
}

int bucket_reader::read(const request::ptr &req, xml::element_list *keys, xml::element_list *prefixes)
{
  int r;
  xml::document doc;
  string temp;
  string query;

  if (!keys)
    return -EINVAL;

  req->init(base::HTTP_GET);

  if (!_truncated)
    return 0;

  query = string("prefix=") + request::url_encode(_prefix) + "&marker=" + _marker;

  if (_group_common_prefixes)
    query += "&delimiter=/";

  if (_max_keys > 0)
    query += string("&max-keys=") + lexical_cast<string>(_max_keys);

  req->set_url(service::get_bucket_url(), query);
  req->run();

  if (req->get_response_code() != base::HTTP_SC_OK)
    return -EIO;

  doc = xml::parse(req->get_output_string());

  if (!doc) {
    S3_LOG(LOG_WARNING, "bucket_reader::read", "failed to parse response.\n");
    return -EIO;
  }

  if ((r = xml::find(doc, IS_TRUNCATED_XPATH, &temp)))
    return r;

  _truncated = (temp == "true");

  if (prefixes && (r = xml::find(doc, PREFIX_XPATH, prefixes)))
    return r;

  if ((r = xml::find(doc, KEY_XPATH, keys)))
    return r;

  if (_truncated) {
    if (service::is_next_marker_supported()) {
      if ((r = xml::find(doc, NEXT_MARKER_XPATH, &_marker)))
        return r;
    } else {
      _marker = keys->back();
    }
  }

  return keys->size() + (prefixes ? prefixes->size() : 0);
}

