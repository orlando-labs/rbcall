#ifndef SPUTNIK_RBCALL_ERROR_NAMESPACE
#define SPUTNIK_RBCALL_ERROR_NAMESPACE "Rbcall"
#endif
#include "runtime/sputnik_ext.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <dlfcn.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <ruby.h>
#include <ruby/encoding.h>
#include <ruby/thread.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
// No Sputnik values or runtime calls cross onto the Ruby thread. Messages own
// their data; foreign objects are represented by ids in Ruby's GC-rooted table.
struct Wire {
  enum Kind {
    Null,
    Bool,
    Int,
    Float,
    String,
    Bytes,
    Symbol,
    Array,
    Map,
    Object
  } kind = Null;
  int64_t integer = 0;
  double number = 0;
  std::string text;
  std::vector<Wire> items;
};
struct HostFault {};
struct CallbackUnwind {};
struct Handle {
  uint64_t id;
  Handle *next = nullptr;
};
std::mutex release_mutex;
Handle *released = nullptr;

std::string string_value(SputnikCtx *cx, SputnikValue value) {
  const char *data;
  size_t size;
  if (!sputnik_str_view(cx, value, &data, &size))
    throw std::runtime_error("expected a string");
  return {data, size};
}
Wire read_packet(SputnikCtx *cx, SputnikValue packet, unsigned depth = 0) {
  if (depth > 64 || !sputnik_is_list(cx, packet) ||
      sputnik_list_len(cx, packet) != 2)
    throw std::runtime_error("invalid Ruby conversion packet");
  int64_t tag;
  if (!sputnik_as_int(cx, sputnik_list_at(cx, packet, 0), &tag))
    throw std::runtime_error("invalid Ruby conversion tag");
  auto value = sputnik_list_at(cx, packet, 1);
  Wire result;
  if (tag == 1) {
    void *raw = nullptr;
    if (!sputnik_handle_ptr(cx, value, "rbcall.Handle", &raw))
      throw HostFault{};
    result.kind = Wire::Object;
    result.integer = static_cast<Handle *>(raw)->id;
  } else if (tag == 4) {
    result.kind = Wire::Symbol;
    result.text = string_value(cx, value);
  } else if (tag == 2 || tag == 3) {
    result.kind = tag == 2 ? Wire::Array : Wire::Map;
    for (size_t i = 0; i < sputnik_list_len(cx, value); ++i) {
      auto entry = sputnik_list_at(cx, value, i);
      if (tag == 2)
        result.items.push_back(read_packet(cx, entry, depth + 1));
      else {
        result.items.push_back(
            read_packet(cx, sputnik_list_at(cx, entry, 0), depth + 1));
        result.items.push_back(
            read_packet(cx, sputnik_list_at(cx, entry, 1), depth + 1));
      }
    }
  } else if (tag == 0) {
    int boolean;
    int64_t integer;
    double number;
    const uint8_t *bytes;
    size_t count;
    if (sputnik_is_null(cx, value)) {
    } else if (sputnik_as_bool(cx, value, &boolean)) {
      result.kind = Wire::Bool;
      result.integer = boolean;
    } else if (sputnik_as_int(cx, value, &integer)) {
      result.kind = Wire::Int;
      result.integer = integer;
    } else if (sputnik_as_float(cx, value, &number)) {
      result.kind = Wire::Float;
      result.number = number;
    } else if (sputnik_is_str(cx, value)) {
      result.kind = Wire::String;
      result.text = string_value(cx, value);
    } else if (sputnik_bytes_view(cx, value, &bytes, &count)) {
      result.kind = Wire::Bytes;
      result.text.assign(reinterpret_cast<const char *>(bytes), count);
    } else
      throw std::runtime_error(
          "unsupported Ruby argument; use a Ruby object or simple values");
  } else
    throw std::runtime_error("unsupported Ruby conversion tag");
  return result;
}
SputnikValue packet(SputnikCtx *cx, const Wire &value) {
  SputnikValue result = nullptr;
  int tag = 0;
  switch (value.kind) {
  case Wire::Null:
    result = sputnik_make_null(cx);
    break;
  case Wire::Bool:
    result = sputnik_make_bool(cx, value.integer);
    break;
  case Wire::Int:
    result = sputnik_make_int(cx, value.integer);
    break;
  case Wire::Float:
    result = sputnik_make_float(cx, value.number);
    break;
  case Wire::String:
    result = sputnik_make_str(cx, value.text.data(), value.text.size());
    break;
  case Wire::Bytes:
    result = sputnik_make_bytes(
        cx, reinterpret_cast<const uint8_t *>(value.text.data()),
        value.text.size());
    break;
  case Wire::Symbol:
    tag = 4;
    result = sputnik_make_str(cx, value.text.data(), value.text.size());
    break;
  case Wire::Object: {
    auto *handle = new Handle{static_cast<uint64_t>(value.integer)};
    result = sputnik_make_handle(cx, "rbcall.Handle", handle);
    if (!result) {
      delete handle;
      throw std::runtime_error("cannot adopt Ruby object");
    }
    tag = 1;
    break;
  }
  default:
    throw std::runtime_error("unexpected Ruby result packet");
  }
  SputnikValue fields[] = {sputnik_make_int(cx, tag), result};
  return sputnik_make_list(cx, fields, 2);
}

struct RubyFailure {
  VALUE exception;
};
// Every potentially raising Ruby API is protected before control can skip a
// C++ destructor. The only deliberate non-local jumps are in yield_callback,
// which has only trivial automatic variables at its exit points.
template <class F> VALUE api(F body) {
  int state = 0;
  VALUE result = rb_protect(
      [](VALUE opaque) -> VALUE { return (*reinterpret_cast<F *>(opaque))(); },
      reinterpret_cast<VALUE>(&body), &state);
  if (state) {
    VALUE error = rb_errinfo();
    rb_set_errinfo(Qnil);
    throw RubyFailure{error};
  }
  return result;
}
VALUE roots = Qnil;
uint64_t next_id = 1;
Wire root_object(VALUE object) {
  Wire result;
  result.kind = Wire::Object;
  result.integer = next_id++;
  api([&] { return rb_hash_aset(roots, ULL2NUM(result.integer), object); });
  return result;
}
VALUE object_id(uint64_t id) {
  VALUE value =
      api([&] { return rb_hash_lookup2(roots, ULL2NUM(id), Qundef); });
  if (value == Qundef)
    throw std::runtime_error("Ruby object has been released");
  return value;
}
VALUE to_ruby(const Wire &value) {
  switch (value.kind) {
  case Wire::Null:
    return Qnil;
  case Wire::Bool:
    return value.integer ? Qtrue : Qfalse;
  case Wire::Int:
    return api([&] { return LL2NUM(value.integer); });
  case Wire::Float:
    return api([&] { return DBL2NUM(value.number); });
  case Wire::String:
    return api(
        [&] { return rb_utf8_str_new(value.text.data(), value.text.size()); });
  case Wire::Bytes:
    return api(
        [&] { return rb_str_new(value.text.data(), value.text.size()); });
  case Wire::Symbol:
    return api([&] {
      return ID2SYM(rb_intern2(value.text.data(), value.text.size()));
    });
  case Wire::Object:
    return object_id(value.integer);
  case Wire::Array: {
    VALUE array = api([] { return rb_ary_new(); });
    for (const auto &item : value.items) {
      VALUE entry = to_ruby(item);
      api([&] { return rb_ary_push(array, entry); });
    }
    RB_GC_GUARD(array);
    return array;
  }
  case Wire::Map: {
    VALUE hash = api([] { return rb_hash_new(); });
    for (size_t i = 0; i < value.items.size(); i += 2) {
      VALUE key = to_ruby(value.items[i]), entry = to_ruby(value.items[i + 1]);
      api([&] { return rb_hash_aset(hash, key, entry); });
    }
    RB_GC_GUARD(hash);
    return hash;
  }
  }
  return Qnil;
}
Wire from_ruby(VALUE object, bool force_object = false) {
  if (force_object)
    return root_object(object);
  Wire result;
  if (NIL_P(object))
    return result;
  if (object == Qtrue || object == Qfalse) {
    result.kind = Wire::Bool;
    result.integer = object == Qtrue;
  } else if (RB_INTEGER_TYPE_P(object)) {
    try {
      result.kind = Wire::Int;
      result.integer = NUM2LL(api([&] { return LL2NUM(NUM2LL(object)); }));
    } catch (const RubyFailure &) {
      return root_object(object);
    }
  } else if (RB_TYPE_P(object, T_FLOAT)) {
    result.kind = Wire::Float;
    result.number = RFLOAT_VALUE(object);
  } else if (SYMBOL_P(object)) {
    result.kind = Wire::Symbol;
    VALUE name = api([&] { return rb_sym2str(object); });
    result.text.assign(RSTRING_PTR(name), RSTRING_LEN(name));
  } else if (RB_TYPE_P(object, T_STRING)) {
    result.kind = rb_enc_get_index(object) == rb_ascii8bit_encindex()
                      ? Wire::Bytes
                      : Wire::String;
    VALUE string = result.kind == Wire::Bytes ? object : api([&] {
      return rb_str_export_to_enc(object, rb_utf8_encoding());
    });
    result.text.assign(RSTRING_PTR(string), RSTRING_LEN(string));
  } else
    return root_object(object);
  return result;
}

struct CallbackTarget {
  std::atomic<bool> active{true};
};
struct CallbackData {
  uint64_t id;
  int64_t required_args;
  std::shared_ptr<CallbackTarget> target;
  CallbackData *next = nullptr;
};
std::mutex callbacks_mutex;
CallbackData *released_callbacks = nullptr;
void callback_free(void *raw) {
  auto *data = static_cast<CallbackData *>(raw);
  std::lock_guard<std::mutex> lock(callbacks_mutex);
  data->next = released_callbacks;
  released_callbacks = data;
}

const rb_data_type_t callback_type = {
    "Sputnik::Callback",
    {nullptr, callback_free, nullptr, nullptr, {nullptr}},
    nullptr,
    nullptr,
    RUBY_TYPED_FREE_IMMEDIATELY};

struct Job {
  std::function<Wire()> body;
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  bool callback_pending = false;
  std::atomic<bool> callback_ready{false};
  Wire result, callback_result;
  std::vector<Wire> callback_args;
  std::string error;
  uint64_t callback_id = 0;
  std::shared_ptr<CallbackTarget> target = std::make_shared<CallbackTarget>();
  SputnikStatus callback_status = SPUTNIK_OK;
  bool aborted = false;
};
struct Executor {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::shared_ptr<Job>> jobs;
  std::string startup_error;
};
// Deliberately process-lived: Ruby objects and the interpreter must outlive
// collector queues. Repeated finalization/reinitialization is not supported.
Executor &executor() {
  static auto *state = new Executor;
  return *state;
}
thread_local Job *active_job = nullptr;
void execute(const std::shared_ptr<Job> &job) {
  Job *previous = active_job;
  active_job = job.get();
  try {
    Handle *pending;
    {
      std::lock_guard<std::mutex> lock(release_mutex);
      pending = released;
      released = nullptr;
    }
    while (pending) {
      Handle *next = pending->next;
      const auto id = pending->id;
      api([&] { return rb_hash_delete(roots, ULL2NUM(id)); });
      delete pending;
      pending = next;
    }
    job->result = job->body();
  } catch (const RubyFailure &failure) {
    try {
      VALUE error = api([&] {
        return rb_funcall(failure.exception, rb_intern("full_message"), 0);
      });
      job->error.assign(RSTRING_PTR(error), RSTRING_LEN(error));
    } catch (...) {
      job->error = "Ruby operation failed";
    }
  } catch (const std::exception &error) {
    job->error = error.what();
  } catch (...) {
    job->error = "Ruby bridge operation failed";
  }
  job->target->active.store(false);
  active_job = previous;
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    job->done = true;
  }
  job->changed.notify_all();
}
std::shared_ptr<Job> pop_job() {
  auto &state = executor();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.jobs.empty())
    return {};
  auto job = state.jobs.front();
  state.jobs.pop_front();
  return job;
}
void *wait_for_work(void *opaque) {
  auto *callback = static_cast<Job *>(opaque);
  auto &state = executor();
  std::unique_lock<std::mutex> lock(state.mutex);
  state.changed.wait(lock, [&] {
    return !state.jobs.empty() || (callback && callback->callback_ready.load());
  });
  return nullptr;
}
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Hold a loader reference while the process-lived Ruby carrier runs.
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&initialize), &info) && info.dli_fname)
      (void)dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL);
    auto ready = std::make_shared<std::promise<void>>();
    auto future = ready->get_future();
    std::thread([ready] {
      try {
        int argc = 1;
        char program[] = "sputnik-rbcall";
        char *argv_data[] = {program, nullptr};
        char **argv = argv_data;
        ruby_sysinit(&argc, &argv);
        RUBY_INIT_STACK;
        ruby_init();
        const char *options[] = {"sputnik-rbcall", "-e", ""};
        ruby_options(3, const_cast<char **>(options));
        const char *gemfile = std::getenv("SPUTNIK_RBCALL_BUNDLE_GEMFILE");
        if (gemfile && *gemfile) {
          api([&] { return rb_require("bundler/setup"); });
        }
        roots = rb_hash_new();
        rb_gc_register_address(&roots);
        ready->set_value();
        for (;;) {
          if (auto job = pop_job())
            execute(job);
          else {
            try {
              api([] {
                rb_thread_call_without_gvl(wait_for_work, nullptr, RUBY_UBF_IO,
                                           nullptr);
                return Qnil;
              });
            } catch (...) {
            }
          }
        }
      } catch (const RubyFailure &failure) {
        std::string message = "cannot initialize worker bundle";
        try {
          VALUE text = api([&] {
            return rb_funcall(failure.exception, rb_intern("full_message"), 0);
          });
          message.assign(RSTRING_PTR(text), RSTRING_LEN(text));
        } catch (...) {
        }
        ready->set_exception(
            std::make_exception_ptr(std::runtime_error(message)));
      } catch (...) {
        ready->set_exception(std::current_exception());
      }
    }).detach();
    future.get();
  });
}

VALUE yield_callback(RB_BLOCK_CALL_FUNC_ARGLIST(yielded, unused)) {
  (void)yielded;
  (void)blockarg;
  auto *data = static_cast<CallbackData *>(RTYPEDDATA_DATA(unused));
  Job *job = active_job;
  if (!job || job->aborted)
    rb_raise(rb_eLocalJumpError, "no active Sputnik callback");
  bool marshal_failed = false;
  try {
    job->callback_id = data->id;
    job->callback_args.clear();
    if (argc == 1 && RB_TYPE_P(argv[0], T_ARRAY) && data->required_args > 1) {
      for (long i = 0; i < RARRAY_LEN(argv[0]); ++i)
        job->callback_args.push_back(from_ruby(rb_ary_entry(argv[0], i)));
    } else {
      for (int i = 0; i < argc; ++i)
        job->callback_args.push_back(from_ruby(argv[i]));
    }
    while (static_cast<int64_t>(job->callback_args.size()) <
           data->required_args)
      job->callback_args.push_back(Wire{});
  } catch (...) {
    marshal_failed = true;
  }
  if (marshal_failed)
    rb_raise(rb_eRuntimeError, "cannot marshal Ruby yield arguments");
  job->callback_ready.store(false);
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    job->callback_pending = true;
  }
  job->changed.notify_all();
  while (!job->callback_ready.load()) {
    VALUE wait_error = Qnil;
    try {
      if (auto nested = pop_job())
        execute(nested);
      else
        api([&] {
          rb_thread_call_without_gvl(wait_for_work, job, RUBY_UBF_IO, nullptr);
          return Qnil;
        });
    } catch (const RubyFailure &failure) {
      wait_error = failure.exception;
    }
    if (!NIL_P(wait_error))
      rb_exc_raise(wait_error);
  }
  if (job->callback_status == SPUTNIK_ERR) {
    job->aborted = true;
    rb_exc_raise(rb_exc_new_cstr(rb_eException, "Sputnik callback unwinding"));
  }
  VALUE value = Qnil;
  marshal_failed = false;
  try {
    value = to_ruby(job->callback_result);
  } catch (...) {
    marshal_failed = true;
  }
  if (marshal_failed)
    rb_raise(rb_eRuntimeError, "cannot marshal Sputnik block result");
  if (job->callback_status == SPUTNIK_BREAK) {
    if (!data->target->active.load())
      rb_raise(rb_eLocalJumpError, "Ruby break target is no longer active");
    rb_iter_break_value(value);
  }
  return value;
}

Wire dispatch(SputnikCtx *cx, std::function<Wire()> body,
              SputnikValue callback = nullptr, SputnikValue encoder = nullptr) {
  initialize();
  auto job = std::make_shared<Job>();
  job->body = std::move(body);
  auto &state = executor();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.jobs.push_back(job);
  }
  state.changed.notify_all();
  std::unique_lock<std::mutex> lock(job->mutex);
  for (;;) {
    job->changed.wait(lock, [&] { return job->done || job->callback_pending; });
    if (job->done)
      break;
    job->callback_pending = false;
    lock.unlock();
    SputnikStatus status = SPUTNIK_ERR;
    Wire reply;
    try {
      if (!callback || sputnik_is_null(cx, callback))
        throw std::runtime_error("Ruby invoked an absent Sputnik block");
      std::vector<SputnikValue> arguments;
      for (const auto &arg : job->callback_args)
        arguments.push_back(packet(cx, arg));
      SputnikValue input = sputnik_make_list(cx, arguments.data(),
                                             arguments.size()),
                   output = nullptr;
      SputnikValue callback_args[] = {sputnik_make_int(cx, job->callback_id),
                                      input};
      status = sputnik_call_ruby_block(cx, callback, callback_args, 2, &output);
      if (status == SPUTNIK_BREAK) {
        SputnikValue packed = nullptr;
        if (sputnik_call_block(cx, encoder, &output, 1, &packed) != SPUTNIK_OK)
          status = SPUTNIK_ERR;
        else
          reply = read_packet(cx, packed);
      } else if (status == SPUTNIK_OK)
        reply = read_packet(cx, output);
    } catch (const HostFault &) {
      status = SPUTNIK_ERR;
    } catch (const std::exception &error) {
      if (status != SPUTNIK_ERR)
        sputnik_fault(cx, SPUTNIK_RBCALL_ERROR_NAMESPACE ".Error", error.what());
      status = SPUTNIK_ERR;
    } catch (...) {
      sputnik_fault(cx, SPUTNIK_RBCALL_ERROR_NAMESPACE ".Error", "Sputnik callback marshalling failed");
      status = SPUTNIK_ERR;
    }
    lock.lock();
    job->callback_status = status;
    job->callback_result = std::move(reply);
    {
      std::lock_guard<std::mutex> state_lock(state.mutex);
      job->callback_ready.store(true);
    }
    state.changed.notify_all();
  }
  if (job->aborted)
    throw CallbackUnwind{};
  if (!job->error.empty())
    throw std::runtime_error(job->error);
  return std::move(job->result);
}
template <class F>
SputnikStatus run(SputnikCtx *cx, SputnikValue *out, F body) {
  try {
    *out = body();
    return *out ? SPUTNIK_OK : SPUTNIK_ERR;
  } catch (const HostFault &) {
    return SPUTNIK_ERR;
  } catch (const CallbackUnwind &) {
    return SPUTNIK_ERR;
  } catch (const std::exception &error) {
    return sputnik_fault(cx, SPUTNIK_RBCALL_ERROR_NAMESPACE ".Error", error.what());
  } catch (...) {
    return sputnik_fault(cx, SPUTNIK_RBCALL_ERROR_NAMESPACE ".Error", "Ruby bridge failed");
  }
}
void arity(size_t n, size_t expected) {
  if (n != expected)
    throw std::runtime_error("invalid rbcall native arity");
}
Wire handle_value(SputnikCtx *cx, SputnikValue value) {
  SputnikValue fields[] = {sputnik_make_int(cx, 1), value};
  return read_packet(cx, sputnik_make_list(cx, fields, 2));
}
VALUE constant(VALUE owner, const std::string &name) {
  return api([&] {
    return rb_const_get(owner, rb_intern2(name.data(), name.size()));
  });
}
} // namespace

extern "C" {
SputnikStatus rbcall_block_arity(SputnikCtx *cx, const SputnikValue *a,
                                 size_t n, SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    return sputnik_make_int(cx, sputnik_block_required_args(cx, a[0]));
  });
}
SputnikStatus rbcall_released_callbacks(SputnikCtx *cx, const SputnikValue *,
                                        size_t n, SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 0);
    CallbackData *pending;
    {
      std::lock_guard<std::mutex> lock(callbacks_mutex);
      pending = released_callbacks;
      released_callbacks = nullptr;
    }
    std::vector<SputnikValue> ids;
    while (pending) {
      auto *next = pending->next;
      ids.push_back(sputnik_make_int(cx, pending->id));
      delete pending;
      pending = next;
    }
    return sputnik_make_list(cx, ids.data(), ids.size());
  });
}
void rbcall_release(void *raw) {
  auto *handle = static_cast<Handle *>(raw);
  std::lock_guard<std::mutex> lock(release_mutex);
  handle->next = released;
  released = handle;
}
SputnikStatus rbcall_import(SputnikCtx *cx, const SputnikValue *a, size_t n,
                            SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    auto path = string_value(cx, a[0]);
    for (auto &ch : path)
      if (ch == '.')
        ch = '/';
    auto result = dispatch(cx, [path] {
      api([&] { return rb_require(path.c_str()); });
      return Wire{};
    });
    return packet(cx, result);
  });
}
SputnikStatus rbcall_eval(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 3);
    auto source = string_value(cx, a[0]);
    return packet(cx, dispatch(
                          cx,
                          [source] {
                            return from_ruby(api([&] {
                              return rb_eval_string(source.c_str());
                            }));
                          },
                          a[1], a[2]));
  });
}
SputnikStatus rbcall_value(SputnikCtx *cx, const SputnikValue *a, size_t n,
                           SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    auto value = read_packet(cx, a[0]);
    return packet(
        cx, dispatch(cx, [value] { return from_ruby(to_ruby(value), true); }));
  });
}
SputnikStatus rbcall_get(SputnikCtx *cx, const SputnikValue *a, size_t n,
                         SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 2);
    auto name = string_value(cx, a[1]);
    Wire owner;
    if (!sputnik_is_null(cx, a[0]))
      owner = handle_value(cx, a[0]);
    return packet(cx, dispatch(cx, [owner, name] {
                    VALUE receiver =
                        owner.kind == Wire::Null ? rb_cObject : to_ruby(owner);
                    return from_ruby(constant(receiver, name));
                  }));
  });
}
SputnikStatus rbcall_send(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 8);
    auto receiver = handle_value(cx, a[0]);
    auto name = string_value(cx, a[1]);
    std::vector<Wire> args;
    std::vector<std::pair<std::string, Wire>> kwargs;
    for (size_t i = 0; i < sputnik_list_len(cx, a[2]); ++i)
      args.push_back(read_packet(cx, sputnik_list_at(cx, a[2], i)));
    for (size_t i = 0; i < sputnik_list_len(cx, a[3]); ++i) {
      auto entry = sputnik_list_at(cx, a[3], i);
      kwargs.emplace_back(string_value(cx, sputnik_list_at(cx, entry, 0)),
                          read_packet(cx, sputnik_list_at(cx, entry, 1)));
    }
    int64_t callback_id = 0, callback_arity = -1;
    if (!sputnik_as_int(cx, a[4], &callback_id) ||
        !sputnik_as_int(cx, a[5], &callback_arity))
      throw std::runtime_error("expected callback id");
    bool has_block = callback_id != 0;
    auto result = dispatch(
        cx,
        [receiver, name, args, kwargs, has_block, callback_id, callback_arity] {
          VALUE self = to_ruby(receiver);
          if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z' &&
              args.empty() && kwargs.empty() && !has_block)
            return from_ruby(constant(self, name));
          VALUE arguments = api([] { return rb_ary_new(); });
          for (const auto &arg : args) {
            VALUE value = to_ruby(arg);
            api([&] { return rb_ary_push(arguments, value); });
          }
          if (!kwargs.empty()) {
            VALUE keywords = api([] { return rb_hash_new(); });
            for (const auto &entry : kwargs) {
              VALUE key = api([&] {
                return ID2SYM(
                    rb_intern2(entry.first.data(), entry.first.size()));
              });
              VALUE value = to_ruby(entry.second);
              api([&] { return rb_hash_aset(keywords, key, value); });
            }
            api([&] { return rb_ary_push(arguments, keywords); });
          }
          const std::string method =
              name == "__call__" ? (RB_TYPE_P(self, T_CLASS) ? "new" : "call")
                                 : name;
          ID selector = rb_intern2(method.data(), method.size());
          VALUE callback_data = Qnil;
          if (has_block) {
            auto *data = new CallbackData{static_cast<uint64_t>(callback_id),
                                          callback_arity, active_job->target};
            try {
              callback_data = api([&] {
                return TypedData_Wrap_Struct(rb_cObject, &callback_type, data);
              });
            } catch (...) {
              delete data;
              throw;
            }
          }
          VALUE value = api([&] {
            return rb_block_call_kw(
                self, selector, static_cast<int>(RARRAY_LEN(arguments)),
                RARRAY_CONST_PTR(arguments),
                has_block ? yield_callback : nullptr, callback_data,
                kwargs.empty() ? RB_NO_KEYWORDS : RB_PASS_KEYWORDS);
          });
          RB_GC_GUARD(arguments);
          return from_ruby(value);
        },
        a[6], a[7]);
    return packet(cx, result);
  });
}
SputnikStatus rbcall_set(SputnikCtx *cx, const SputnikValue *a, size_t n,
                         SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 3);
    auto receiver = handle_value(cx, a[0]);
    auto name = string_value(cx, a[1]) + "=";
    auto value = read_packet(cx, a[2]);
    return packet(cx, dispatch(cx, [receiver, name, value] {
                    VALUE arg = to_ruby(value), self = to_ruby(receiver);
                    return from_ruby(api([&] {
                      return rb_funcallv(self, rb_intern(name.c_str()), 1,
                                         &arg);
                    }));
                  }));
  });
}
SputnikStatus rbcall_item(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 2);
    auto receiver = handle_value(cx, a[0]), key = read_packet(cx, a[1]);
    return packet(cx, dispatch(cx, [receiver, key] {
                    VALUE arg = to_ruby(key), self = to_ruby(receiver);
                    return from_ruby(api([&] {
                      return rb_funcallv(self, rb_intern("[]"), 1, &arg);
                    }));
                  }));
  });
}
SputnikStatus rbcall_set_item(SputnikCtx *cx, const SputnikValue *a, size_t n,
                              SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 3);
    auto receiver = handle_value(cx, a[0]), key = read_packet(cx, a[1]),
         value = read_packet(cx, a[2]);
    return packet(cx, dispatch(cx, [receiver, key, value] {
                    VALUE args[] = {to_ruby(key), to_ruby(value)},
                          self = to_ruby(receiver);
                    return from_ruby(api([&] {
                      return rb_funcallv(self, rb_intern("[]="), 2, args);
                    }));
                  }));
  });
}
SputnikStatus rbcall_sequence(SputnikCtx *cx, const SputnikValue *a, size_t n,
                              SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    auto receiver = handle_value(cx, a[0]);
    auto result = dispatch(cx, [receiver] {
      VALUE self = to_ruby(receiver);
      VALUE array = api([&] { return rb_funcall(self, rb_intern("to_a"), 0); });
      Wire result;
      result.kind = Wire::Array;
      for (long i = 0; i < RARRAY_LEN(array); ++i)
        result.items.push_back(from_ruby(rb_ary_entry(array, i)));
      RB_GC_GUARD(array);
      return result;
    });
    std::vector<SputnikValue> items;
    for (auto &value : result.items)
      items.push_back(packet(cx, value));
    return sputnik_make_list(cx, items.data(), items.size());
  });
}
SputnikStatus rbcall_entries(SputnikCtx *cx, const SputnikValue *a, size_t n,
                             SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    auto receiver = handle_value(cx, a[0]);
    auto result = dispatch(cx, [receiver] {
      VALUE self = to_ruby(receiver);
      VALUE entries =
          api([&] { return rb_funcall(self, rb_intern("to_a"), 0); });
      Wire result;
      result.kind = Wire::Array;
      for (long i = 0; i < RARRAY_LEN(entries); ++i) {
        VALUE pair = rb_ary_entry(entries, i);
        if (!RB_TYPE_P(pair, T_ARRAY) || RARRAY_LEN(pair) != 2)
          throw std::runtime_error("expected Ruby key/value pairs");
        result.items.push_back(from_ruby(rb_ary_entry(pair, 0)));
        result.items.push_back(from_ruby(rb_ary_entry(pair, 1)));
      }
      RB_GC_GUARD(entries);
      return result;
    });
    std::vector<SputnikValue> entries;
    for (size_t i = 0; i < result.items.size(); i += 2) {
      SputnikValue pair[] = {packet(cx, result.items[i]),
                             packet(cx, result.items[i + 1])};
      entries.push_back(sputnik_make_list(cx, pair, 2));
    }
    return sputnik_make_list(cx, entries.data(), entries.size());
  });
}
SputnikStatus rbcall_repr(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    auto receiver = handle_value(cx, a[0]);
    auto result = dispatch(cx, [receiver] {
      VALUE self = to_ruby(receiver);
      return from_ruby(api([&] { return rb_inspect(self); }));
    });
    return sputnik_make_str(cx, result.text.data(), result.text.size());
  });
}
}
