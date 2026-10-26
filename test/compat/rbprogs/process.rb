r, w = IO.pipe
pid = fork { r.close; w.puts "from child #{Process.pid != Process.ppid}"; w.close; exit 3 }
w.close; puts r.read
Process.wait(pid); p $?.exitstatus
pid = fork { sleep 10 }
Process.kill("TERM", pid); Process.wait(pid); p $?.signaled?, $?.termsig
Signal.trap("USR1") { puts "got usr1" }
Process.kill("USR1", Process.pid); sleep 0.1
p `echo shell`.chomp, system("exit 2"), $?.exitstatus
