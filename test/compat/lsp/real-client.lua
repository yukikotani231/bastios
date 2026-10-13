-- Starts lua-language-server with Neovim's LSP client on /root/luaproj,
-- asks for the hover on a local variable, stops the client, and writes what
-- happened to /root/lsreal.txt.
local out = {}
local file = "/root/luaproj/main.lua"
vim.cmd.edit(file)
local buf = vim.api.nvim_get_current_buf()
local id = vim.lsp.start({
  name = "lua-ls",
  cmd = { "lua-language-server", "--logpath=/root/ls-log", "--metapath=/root/ls-meta" },
  root_dir = "/root/luaproj",
}, { bufnr = buf })
vim.wait(60000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local reply
-- `local answer = 42`: the cursor is on `answer`; the server loads its
-- workspace first, so it is asked again until it has an answer.
vim.wait(60000, function()
  c:request("textDocument/hover", { textDocument = { uri = vim.uri_from_bufnr(buf) }, position = { line = 0, character = 8 } },
    function(_, r) reply = r end, buf)
  vim.wait(2000, function() return reply ~= nil end, 50)
  return reply ~= nil
end, 100)
local text = reply and reply.contents and (reply.contents.value or vim.inspect(reply.contents)) or "no-reply"
out[#out + 1] = text:find("answer: integer = 42", 1, true) and "hover-integer" or text
c:stop()
vim.wait(20000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/lsreal.txt", "w"):write(table.concat(out, ","), "\n")
