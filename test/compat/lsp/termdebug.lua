-- Runs Neovim's Termdebug plugin (gdb over its MI interface in terminal
-- windows) on /root/d (built from /root/d.c in /root), a PIE program built with -g: sets a breakpoint in
-- `add`, runs the program to it, checks the stop in gdb's output and the
-- signs in the source buffer, prints an expression and quits. What happened
-- goes to /root/termdebug.txt. TERMDEBUGGER is the gdb to start.
local out = {}
vim.cmd("packadd termdebug")
vim.g.termdebugger = os.getenv("TERMDEBUGGER") or "gdb"
vim.cmd("cd /root")
vim.cmd("edit d.c")
vim.cmd("Termdebug ./d")
local function gdbtext()
  local t = {}
  for _, b in ipairs(vim.api.nvim_list_bufs()) do
    if vim.bo[b].buftype == "terminal" then
      t[#t + 1] = table.concat(vim.api.nvim_buf_get_lines(b, 0, -1, false), "\n")
    end
  end
  return table.concat(t, "\n")
end
local function seen(text, ms)
  return vim.wait(ms, function() return gdbtext():find(text, 1, true) ~= nil end, 100)
end
out[#out + 1] = seen("(gdb)", 120000) and "gdb-up" or "no-gdb"
vim.cmd("call TermDebugSendCommand('break add')")
vim.cmd("call TermDebugSendCommand('run')")
out[#out + 1] = seen("Breakpoint 1, add (a=40, b=2)", 120000) and "stopped-in-add" or "no-stop"
vim.wait(2000)
local signs = {}
for _, sg in ipairs(vim.fn.sign_getplaced(vim.fn.bufnr("d.c"), { group = "*" })[1].signs) do
  signs[sg.name:gsub("%d+%.%d+$", "N")] = sg.lnum
end
out[#out + 1] = signs.debugPC == 2 and "pc-line-2" or "no-pc-sign"
out[#out + 1] = signs.debugBreakpointN == 2 and "bp-line-2" or "no-bp-sign"
vim.cmd("call TermDebugSendCommand('print a + b')")
out[#out + 1] = seen("$1 = 42", 60000) and "print-42" or "no-print"
vim.cmd("call TermDebugSendCommand('kill')")
vim.wait(2000)
io.open("/root/termdebug.txt", "w"):write(table.concat(out, ","), "\n")
vim.cmd("qa!")
