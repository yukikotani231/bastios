-- Starts gopls (unpacked under /usr/local) with Neovim's LSP client on the
-- module in /root/goproj, asks for the hover on a call, stops the client, and
-- writes what happened to /root/gopls.txt.
local out = {}
vim.cmd.edit("/root/goproj/main.go")
local buf = vim.api.nvim_get_current_buf()
local id = vim.lsp.start({
  name = "gopls",
  cmd = { "/usr/local/bin/gopls" },
  cmd_env = {
    PATH = "/usr/local/go/bin:/usr/local/bin:/usr/bin:/bin",
    HOME = "/root", GOCACHE = "/tmp/gc", GOPATH = "/tmp/gp",
    GOTOOLCHAIN = "local", GOFLAGS = "-p=1", CGO_ENABLED = "0",
  },
  root_dir = "/root/goproj",
}, { bufnr = buf })
vim.wait(60000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local text = "no-reply"
-- `println(add(1, 2))`, line 5: the cursor is on `add`; the hover is its signature.
vim.wait(120000, function()
  local reply
  c:request("textDocument/hover", { textDocument = { uri = vim.uri_from_bufnr(buf) }, position = { line = 5, character = 10 } },
    function(_, r) reply = r end, buf)
  vim.wait(5000, function() return reply ~= nil end, 50)
  if reply and reply.contents then
    text = reply.contents.value or vim.inspect(reply.contents)
  end
  return text:find("func add(a int, b int) int", 1, true) ~= nil
end, 500)
out[#out + 1] = text:find("func add(a int, b int) int", 1, true) and "hover-func" or text
c:stop(true)
vim.wait(20000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/gopls.txt", "w"):write(table.concat(out, ","), "\n")
