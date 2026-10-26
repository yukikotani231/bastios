m = Mutex.new; n = 0
ts = 4.times.map { Thread.new { 1000.times { m.synchronize { n += 1 } } } }
ts.each(&:join); p n
q = Queue.new
c = Thread.new { s = 0; while (v = q.pop); s += v; end; s }
10.times { |i| q << i }; q << nil; p c.value
f = Fiber.new { Fiber.yield 1; Fiber.yield 2; 3 }
p [f.resume, f.resume, f.resume]
e = Enumerator.new { |y| a, b = 0, 1; loop { y << a; a, b = b, a + b } }
p e.take(10)
