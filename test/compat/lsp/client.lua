-- Starts the stub language server with Neovim's LSP client, asks for a
-- hover, stops the client, and writes what happened to /root/lsp.txt.
local out = {}
local buf = vim.api.nvim_create_buf(true, false)
vim.api.nvim_buf_set_lines(buf, 0, -1, false, { "hello" })
vim.api.nvim_set_current_buf(buf)
local id = vim.lsp.start({
  name = "stub",
  cmd = { "/usr/bin/nvim", "-l", "/root/lsp-server.lua" },
  root_dir = "/root",
}, { bufnr = buf })
vim.wait(10000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 50)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local reply
c:request("textDocument/hover", { textDocument = { uri = vim.uri_from_bufnr(buf) }, position = { line = 0, character = 0 } },
  function(_, r) reply = r end, buf)
vim.wait(10000, function() return reply ~= nil end, 50)
out[#out + 1] = reply and reply.contents.value or "no-reply"
c:stop()
vim.wait(10000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/lsp.txt", "w"):write(table.concat(out, ","), "\n")
