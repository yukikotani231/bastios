-- Starts the JSON language server of VS Code (node, unpacked under
-- /opt/jsonls) with Neovim's LSP client, waits for the diagnostic on a file with
-- a trailing comma, formats a second file with a formatting request,
-- stops the client, and writes what happened to /root/json.txt.
local out = {}
vim.cmd.edit("/root/jsonproj/bad.json")
local bad = vim.api.nvim_get_current_buf()
local id = vim.lsp.start({
  name = "jsonls",
  cmd = { "/usr/bin/node", "/opt/jsonls/node_modules/vscode-langservers-extracted/bin/vscode-json-language-server", "--stdio" },
  root_dir = "/root/jsonproj",
}, { bufnr = bad })
vim.wait(120000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local msg = "no-diagnostic"
vim.wait(60000, function()
  for _, d in ipairs(vim.diagnostic.get(bad)) do
    if d.message == "Trailing comma" then
      msg = "diagnostic"
      return true
    end
  end
  return false
end, 500)
out[#out + 1] = msg
vim.cmd.edit("/root/jsonproj/good.json")
local good = vim.api.nvim_get_current_buf()
vim.bo[good].shiftwidth = 2
vim.bo[good].expandtab = true
vim.lsp.buf_attach_client(good, id)
vim.wait(10000, function() return false end, 1000)
-- The server offers formatting only through dynamic registration, which the
-- client's format() filters on; ask for the edits directly and apply them.
local res = c:request_sync("textDocument/formatting", {
  textDocument = { uri = vim.uri_from_bufnr(good) },
  options = { tabSize = 2, insertSpaces = true },
}, 60000, good)
if res and res.result then
  vim.lsp.util.apply_text_edits(res.result, good, "utf-16")
end
local lines = vim.api.nvim_buf_get_lines(good, 0, -1, false)
out[#out + 1] = (#lines > 1 and lines[2] == '  "a": 1,') and "formatted" or ("unformatted:" .. table.concat(lines, "|"))
c:stop(true)
vim.wait(20000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/json.txt", "w"):write(table.concat(out, ","), "\n")
