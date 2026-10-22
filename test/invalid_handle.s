package rbcall.invalid_handle
import rbcall
export main

def main():
  disposed = rbcall.wrap([1])
  disposed.destroy!()
  rbcall.wrap([1]).map |x|: disposed
