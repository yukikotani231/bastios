-- Starts TypeScript 7's language server (lib/tsc --lsp, unpacked under
-- /opt/typescript) with Neovim's LSP client on the project in /root/tsproj,
-- asks for the hover on a call and for the diagnostics of the file, stops the
-- client, and writes what happened to /root/ts.txt.
local out = {}
vim.cmd.edit("/root/tsproj/main.ts")
local buf = vim.api.nvim_get_current_buf()
local id = vim.lsp.start({
  name = "tsc",
  cmd = { "/opt/typescript/lib/tsc", "--lsp", "--stdio" },
  cmd_env = { HOME = "/root", GOMEMLIMIT = "300MiB" },
  root_dir = "/root/tsproj",
}, { bufnr = buf })
vim.wait(120000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local text = "no-reply"
-- `console.log(add(1, 2))`, line 2: the cursor is on `add`; the hover is its signature.
vim.wait(120000, function()
  local reply
  c:request("textDocument/hover", { textDocument = { uri = vim.uri_from_bufnr(buf) }, position = { line = 2, character = 14 } },
    function(_, r) reply = r end, buf)
  vim.wait(5000, function() return reply ~= nil end, 50)
  if reply and reply.contents then
    text = reply.contents.value or vim.inspect(reply.contents)
  end
  return text:find("function add(a: number, b: number): number", 1, true) ~= nil
end, 500)
out[#out + 1] = text:find("function add(a: number, b: number): number", 1, true) and "hover-func" or text
-- `const s: string = add(1, 2)`, line 1: a type error.
local msg = "no-diagnostic"
vim.wait(60000, function()
  for _, d in ipairs(vim.diagnostic.get(buf)) do
    if d.message:find("is not assignable to type 'string'", 1, true) then
      msg = "diagnostic"
      return true
    end
  end
  return false
end, 500)
out[#out + 1] = msg
c:stop(true)
vim.wait(20000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/ts.txt", "w"):write(table.concat(out, ","), "\n")
