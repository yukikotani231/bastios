require "socket"; require "net/http"
srv = TCPServer.new("127.0.0.1", 0)
port = srv.addr[1]
t = Thread.new { c = srv.accept; c.puts "hello #{c.gets.chomp}"; c.close }
s = TCPSocket.new("127.0.0.1", port); s.puts "ruby"; puts s.read; s.close; t.join
a, b = UNIXSocket.pair; a.write "ping"; a.close; puts b.read
u = UDPSocket.new; u.bind("127.0.0.1", 0); v = UDPSocket.new; v.send("dgram", 0, "127.0.0.1", u.addr[1]); puts u.recvfrom(16)[0]
http = TCPServer.new("127.0.0.1", 0)
ht = Thread.new { c = http.accept; while (l = c.gets) && l != "\r\n"; end; c.write "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"; c.close }
r = Net::HTTP.get_response(URI("http://127.0.0.1:#{http.addr[1]}/")); puts r.code, r.body; ht.join
