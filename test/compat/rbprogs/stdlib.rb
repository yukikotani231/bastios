require "json"; require "yaml"; require "digest"; require "zlib"; require "openssl"; require "set"; require "securerandom"; require "time"; require "stringio"
j = JSON.generate({"a" => [1, 2.5, nil, true], "b" => {"c" => "d"}})
puts j; p JSON.parse(j)["a"]
puts({"x" => [1, {"y" => "z"}]}.to_yaml)
p YAML.load("a: [1, 2]\nb: {c: d}")
puts Digest::SHA256.hexdigest("abc"), Digest::MD5.hexdigest("abc")
z = Zlib::Deflate.deflate("hello " * 100); p z.bytesize < 40, Zlib::Inflate.inflate(z).size
puts OpenSSL::HMAC.hexdigest("SHA256", "key", "msg")
p OpenSSL::Cipher.new("aes-128-cbc").encrypt.tap { |c| c.key = "k" * 16; c.iv = "i" * 16 }.update("x" * 16).bytesize
p SecureRandom.hex(8).size, Set[1, 2] | Set[2, 3]
t = Time.utc(2026, 10, 25, 12, 30, 5); puts t.iso8601, Time.parse("2026-01-02 03:04:05 UTC").to_i
