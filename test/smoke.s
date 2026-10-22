package rbcall.smoke
from rbcall.json import JSON as json
from rbcall.tempfile import Tempfile
from rbcall.date import Date
from rbcall.net.http import Net
import rbcall
import task
export main

def check(value):
  raise ValueError("rbcall assertion failed") unless value

def main():
  check(json.generate([1, 2, 3]) == "[1,2,3]")
  items = rbcall.wrap([1, 2, 3])
  check(items[1] == 2)
  items[1] = 9
  check(items.to_a == [1, 9, 3])
  mapped = items.map |x|: x * 2
  check(mapped.to_a == [2, 18, 6])
  stopped = items.each |x|:
    break x if x > 5
  check(stopped == 9)
  text = Tempfile.create("sputnik") |file|:
    file.write("hello")
    file.rewind()
    file.read()
  check(text == "hello")
  holder = rbcall.eval("Class.new { def save(&b); @b=b; end; def run(x); @b.call(x); end }.new")
  offset = 10
  holder.save |x|: x + offset
  check(holder.run(5) == 15)
  rbcall.eval("GC.start")
  check(holder.run(7) == 17)
  caught = false
  try:
    json.parse("{")
  rescue rbcall.Error |e|:
    caught = true
  check(caught)
  caught = false
  try:
    items.each |x|: raise ValueError("Sputnik block failure")
  rescue ValueError:
    caught = true
  check(caught)
  caught = false
  try:
    local_items = [1, 2]
    local_items.each |x|: break x
  rescue LocalJumpError:
    caught = true
  check(caught)
  check(return_through_ruby(rbcall.wrap([1, 2, 3])) == 2)
  fixture = rbcall.eval("require File.join(Dir.pwd, 'rbcall/test/fixture'); SputnikBridgeFixture")
  marker = [0]
  stopped = fixture.around |x|:
    try:
      break x + 1
    ensure:
      marker[0] = marker[0] + 1
  check(stopped == 8 and marker[0] == 1 and fixture.ensures == 1 and not fixture.continued)
  caught = false
  try:
    fixture.around |x|: raise ValueError("Sputnik error")
  rescue ValueError:
    caught = true
  check(caught and fixture.ensures == 1 and not fixture.continued)
  fixture.save |x|: break x
  caught = false
  try:
    fixture.run(4)
  rescue rbcall.Error:
    caught = true
  check(caught)
  check(fixture.keywords(value: 6, factor: 7) == 42)
  child = task.spawn:
    local = task.local()
    local.with(77):
      ruby_values = rbcall.wrap([1, 2])
      result = ruby_values.map |x|:
        task.sleep(0.001)
        check(local.get() == 77)
        x + 1
      result.to_a
  check(child.wait() == [2, 3])
  total = [0]
  ruby_hash = rbcall.wrap({one: 1, two: 2})
  ruby_hash.each |key, value|:
    total[0] = total[0] + value
  check(total[0] == 3)
  check(ruby_hash[:one] == 1)
  check(Date(2026, 10, 20).year == 2026)
  check(Net.HTTP.name == "Net::HTTP")
  result = rbcall.wrap([1, 2, 3]).map |x|:
    break :done if x == 2
    x * 2
  check(result == :done)
  result = rbcall.wrap([1, 2, 3]).map |x|:
    if x == 2:
      next 0
    x * 2
  check(result.to_a == [2, 0, 6])
  print("PASS rbcall smoke")
  0


def return_through_ruby(items):
  items.each |x|:
    return x if x == 2
  0
