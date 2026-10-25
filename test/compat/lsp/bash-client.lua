-- Starts the Bash language server (node, unpacked under /opt/bashls) with
-- Neovim's LSP client on a small script, asks where the function called on the
-- last line is defined and for the symbols of the file, stops the client, and
-- writes what happened to /root/bash.txt.
local out = {}
vim.cmd.edit("/root/bashproj/a.sh")
local buf = vim.api.nvim_get_current_buf()
vim.bo[buf].filetype = "sh"
local id = vim.lsp.start({
  name = "bashls",
  cmd = { "/usr/bin/node", "/opt/bashls/node_modules/bash-language-server/out/cli.js", "start" },
  root_dir = "/root/bashproj",
}, { bufnr = buf })
vim.wait(120000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local def = "no-definition"
vim.wait(60000, function()
  local reply
  c:request("textDocument/definition", {
    textDocument = { uri = vim.uri_from_bufnr(buf) },
    position = { line = 3, character = 1 },
  }, function(_, r) reply = r end, buf)
  vim.wait(5000, function() return reply ~= nil end, 50)
  if reply and reply[1] and reply[1].range and reply[1].range.start.line == 0 then
    def = "definition"
    return true
  end
  return false
end, 500)
out[#out + 1] = def
local syms = c:request_sync("textDocument/documentSymbol", { textDocument = { uri = vim.uri_from_bufnr(buf) } }, 30000, buf)
out[#out + 1] = (syms and syms.result and syms.result[1] and syms.result[1].name == "greet") and "symbol" or "no-symbol"
c:stop(true)
vim.wait(20000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/bash.txt", "w"):write(table.concat(out, ","), "\n")
