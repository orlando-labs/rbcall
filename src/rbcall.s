package rbcall

from sync import Mutex

export Object, Error, require, constant, import_name, wrap, eval

class Error < NativeError

native class Handle from "rbcall.Handle" collected:
  def destroy!() from "rbcall_release"

native def _import(path) from "rbcall_import" blocking
native def _eval(source, dispatcher, encoder) from "rbcall_eval" blocking
native def _send(handle, name, args, kwargs, callback_id, callback_arity, dispatcher, encoder) from "rbcall_send" blocking
native def _get(handle, name) from "rbcall_get" blocking
native def _set(handle, name, value) from "rbcall_set" blocking
native def _item(handle, key) from "rbcall_item" blocking
native def _set_item(handle, key, value) from "rbcall_set_item" blocking
native def _value(value) from "rbcall_value" blocking
native def _sequence(handle) from "rbcall_sequence" blocking
native def _entries(handle) from "rbcall_entries" blocking
native def _repr(handle) from "rbcall_repr" blocking
native def _block_arity(block) from "rbcall_block_arity"
native def _released_callbacks() from "rbcall_released_callbacks"

# Protocol packets distinguish data arrays from native object references.
def _pack(value, depth: 0):
  raise ArgumentError("foreign conversion exceeds 64 levels (possible cycle)") if depth > 64
  case value:
    when Object:
      [1, value._handle()]
    when (Array | Tuple):
      [2, value.map |item|: _pack(item, depth: depth + 1)]
    when (Map | StrictMap):
      [3, value.entries.map |entry|:
        [_pack(entry[0], depth: depth + 1), _pack(entry[1], depth: depth + 1)]]
    when Symbol:
      [4, value.to_str]
    else:
      [0, value]

def _unpack(packet):
  if packet[0] == 1:
    Object(packet[1])
  elif packet[0] == 4:
    packet[1].to_symbol
  else:
    packet[1]

def wrap(value): _unpack(_value(_pack(value)))
def eval(source): _unpack(_eval(source, _dispatch, _encode_callback))
def require(path):
  _import(path)
  null
def constant(name): _unpack(_get(null, name.to_str))
def import_name(path, name):
  require(path)
  constant(name)

class Object:
  def init(@handle)
  def _handle(): @handle

  def method_missing(name, *args, **kwargs, &block):
    invoke(name.to_str, *args, **kwargs, &block)

  def invoke(name, *args, **kwargs, &block):
    _cleanup_callbacks()
    packed_args = args.map |arg|: _pack(arg)
    packed_kwargs = kwargs.entries.map |entry|: [entry[0].to_str, _pack(entry[1])]
    callback_id = 0
    callback_arity = -1
    if block:
      callback_id = _register_callback(block)
      callback_arity = _block_arity(block)
    _unpack(_send(@handle, name, packed_args, packed_kwargs, callback_id, callback_arity, _dispatch, _encode_callback))

  def call(*args, **kwargs, &block): invoke("__call__", *args, **kwargs, &block)
  def getattr(name): _unpack(_get(@handle, name.to_str))
  def setattr!(name, value): _unpack(_set(@handle, name.to_str, _pack(value)))
  def [](key): _unpack(_item(@handle, _pack(key)))
  def []=(key, value): _unpack(_set_item(@handle, _pack(key), _pack(value)))
  def to_array(): _sequence(@handle).map |packet|: _unpack(packet)
  def to_a(): to_array()
  def to_map():
    result = {}
    _entries(@handle).each |entry|:
      result[_unpack(entry[0])] = _unpack(entry[1])
    result
  def repr(): _repr(@handle)
  def to_str(): repr()
  def destroy!(): @handle.destroy!()


_callbacks = {}
_callback_counter = [0]
_callback_mutex = Mutex.new()

def _register_callback(block):
  _callback_mutex.synchronize:
    _callback_counter[0] = _callback_counter[0] + 1
    id = _callback_counter[0]
    _callbacks[id] = block
    id

def _dispatch(id, packets):
  block = _callback_mutex.synchronize: _callbacks[id]
  raise LocalJumpError("Ruby callback has been released") unless block
  values = packets.map |packet|: _unpack(packet)
  _pack(block(*values))

def _encode_callback(value): _pack(value)

def _cleanup_callbacks():
  ids = _released_callbacks()
  _callback_mutex.synchronize:
    ids.each |id|: _callbacks.delete!(id)
  null
