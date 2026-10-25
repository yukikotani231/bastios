-- Starts the YAML language server (node, unpacked under /opt/yamlls) with
-- Neovim's LSP client on a file with a broken flow sequence, waits for the
-- diagnostic, asks for the symbols of the file, stops the client, and writes
-- what happened to /root/yaml.txt.
local out = {}
vim.cmd.edit("/root/yamlproj/a.yaml")
local buf = vim.api.nvim_get_current_buf()
vim.bo[buf].filetype = "yaml"
local id = vim.lsp.start({
  name = "yamlls",
  cmd = { "/usr/bin/node", "/opt/yamlls/node_modules/yaml-language-server/bin/yaml-language-server", "--stdio" },
  root_dir = "/root/yamlproj",
}, { bufnr = buf })
vim.wait(120000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local msg = "no-diagnostic"
vim.wait(60000, function()
  for _, d in ipairs(vim.diagnostic.get(buf)) do
    if d.message:find("Flow sequence in block collection", 1, true) then
      msg = "diagnostic"
      return true
    end
  end
  return false
end, 500)
out[#out + 1] = msg
local syms = c:request_sync("textDocument/documentSymbol", { textDocument = { uri = vim.uri_from_bufnr(buf) } }, 30000, buf)
local names = {}
for _, s in ipairs(syms and syms.result or {}) do
  names[#names + 1] = s.name
end
out[#out + 1] = table.concat(names, "+") == "name+items+other" and "symbols" or ("symbols:" .. table.concat(names, "+"))
c:stop(true)
vim.wait(20000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/yaml.txt", "w"):write(table.concat(out, ","), "\n")
