#ifndef S3_OBJECT_CACHE_HH
#define S3_OBJECT_CACHE_HH

#include <map>
#include <string>
#include <boost/smart_ptr.hpp>
#include <boost/thread.hpp>

#include "logging.hh"
#include "object.hh"
#include "thread_pool.hh"

namespace s3
{
  class file_transfer;
  class mutexes;
  class request;

  enum cache_hints
  {
    HINT_NONE    = 0x0,
    HINT_IS_DIR  = 0x1,
    HINT_IS_FILE = 0x2
  };

  class object_cache
  {
  public:
    object_cache(
      const thread_pool::ptr &pool, 
      const boost::shared_ptr<mutexes> &mutexes,
      const boost::shared_ptr<file_transfer> &file_transfer);

    ~object_cache();

    inline object::ptr get(const std::string &path, int hints = HINT_NONE)
    {
      object::ptr obj = find(path);

      if (!obj)
        _pool->call(boost::bind(&object_cache::__fetch, this, _1, path, hints, &obj));

      return obj;
    }

    inline object::ptr get(const boost::shared_ptr<request> &req, const std::string &path, int hints = HINT_NONE)
    {
      object::ptr obj = find(path);

      if (!obj)
        __fetch(req, path, hints, &obj);

      return obj;
    }

    inline void remove(const std::string &path)
    {
      boost::mutex::scoped_lock lock(_mutex);
      cache_map::iterator itor = _cache_map.find(path);

      if (itor == _cache_map.end())
        return;

      // TODO: force-delete the file?
      if (itor->second->get_open_file())
        _handle_map.erase(itor->second->get_open_file()->get_handle());

      _cache_map.erase(path);
    }

    inline open_file::ptr get_file(uint64_t handle)
    {
      boost::mutex::scoped_lock lock(_mutex);
      handle_map::const_iterator itor = _handle_map.find(handle);

      return (itor == _handle_map.end()) ? open_file::ptr() : itor->second->get_open_file();
    }

    int open_handle(const std::string &path, uint64_t *handle)
    {
      boost::mutex::scoped_lock lock(_mutex, boost::defer_lock);
      object::ptr obj = get(path, HINT_IS_FILE);
      open_file::ptr file;

      if (!obj) {
        S3_DEBUG("object_cache::open_handle", "cannot open file [%s].\n", path.c_str());
        return -ENOENT;
      }

      lock.lock();
      file = obj->get_open_file();

      if (!file) {
        uint64_t handle = _next_handle++;
        int r;

        file.reset(new open_file(_mutexes, _file_transfer, obj, handle));
        obj->set_open_file(file);

        // handle needs to be in _handle_map before unlocking because a concurrent call to open_handle() for 
        // the same file will block on add_reference(), which expects to have handle in _handle_map on return.
        _handle_map[handle] = obj;

        lock.unlock();
        r = file->init();
        lock.lock();

        if (r) {
          S3_DEBUG("object_cache::open_handle", "failed to open file [%s] with error %i.\n", obj->get_path().c_str(), r);

          obj->set_open_file(open_file::ptr());
          _handle_map.erase(handle);
          return r;
        }
      }

      return file->add_reference(handle);
    }

    inline int release_handle(uint64_t handle)
    {
      boost::mutex::scoped_lock lock(_mutex);
      handle_map::iterator itor = _handle_map.find(handle);
      object::ptr obj;

      if (itor == _handle_map.end()) {
        S3_DEBUG("object_cache::release_handle", "attempt to release handle not in map.\n");
        return -EINVAL;
      }

      obj = itor->second;

      if (obj->get_open_file()->release()) {
        _handle_map.erase(handle);

        lock.unlock();
        obj->get_open_file()->cleanup();
        lock.lock();

        // keep in _cache_map until cleanup() returns so that any concurrent attempts to open the file fail
        _cache_map.erase(obj->get_path());
        // TODO: verify that this destroys the object!
      }

      return 0;
    }

  private:
    typedef std::map<std::string, object::ptr> cache_map;
    typedef std::map<uint64_t, object::ptr> handle_map;

    inline object::ptr find(const std::string &path)
    {
      boost::mutex::scoped_lock lock(_mutex);
      object::ptr &obj = _cache_map[path];

      if (!obj) {
        _misses++;

      } else if (!obj->get_open_file() && !obj->is_valid()) {
        _expiries++;
        obj.reset(); // no need to remove from _handle_map because get_open_file() is null 

      } else {
        _hits++;
      }

      return obj;
    }

    int __fetch(const request_ptr &req, const std::string &path, int hints, object::ptr *obj);

    cache_map _cache_map;
    handle_map _handle_map;
    boost::mutex _mutex;
    thread_pool::ptr _pool;
    boost::shared_ptr<mutexes> _mutexes;
    boost::shared_ptr<file_transfer> _file_transfer;
    uint64_t _hits, _misses, _expiries;
    uint64_t _next_handle;
  };
}

#endif
