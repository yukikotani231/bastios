Pt = Struct.new(:x, :y) { def dist = Math.sqrt(x*x + y*y) }
p Pt.new(3, 4).dist
h = {b: 2, a: 1}.sort.to_h
p h, h.map { |k, v| "#{k}=#{v}" }.join("&")
puts format("%05.1f|%-4s|%x", 3.14159, "ab", 255)
p (1..10).each_slice(3).map(&:sum), [1, 2, 3].zip([4, 5, 6]).to_h
p "héllo wörld".upcase, "héllo".bytes.size, "abc".succ, 2**100
