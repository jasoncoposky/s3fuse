#ifndef S3_WORK_ITEM_H
#define S3_WORK_ITEM_H

#include <boost/function.hpp>
#include <boost/smart_ptr.hpp>

namespace s3
{
  class async_handle;
  class request;

  class work_item
  {
  public:
    typedef boost::function1<int, boost::shared_ptr<request> > worker_function;

    inline work_item()
    {
    }

    inline work_item(const worker_function &function, const boost::shared_ptr<async_handle> &ah)
      : _function(function), 
        _ah(ah)
    {
    }

    inline bool is_valid() { return _ah; }
    inline const boost::shared_ptr<async_handle> & get_ah() { return _ah; }
    inline const worker_function & get_function() { return _function; }

  private:
    worker_function _function;
    boost::shared_ptr<async_handle> _ah;
  };
}

#endif