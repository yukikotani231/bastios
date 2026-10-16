-- Starts python-lsp-server (python3 -m pylsp, from the tree unpacked in
-- /opt/pylsp) with Neovim's LSP client on /root/pyproj, asks for the hover on
-- a variable, stops the client, and writes what happened to /root/pylsp.txt.
local out = {}
vim.cmd.edit("/root/pyproj/main.py")
local buf = vim.api.nvim_get_current_buf()
local id = vim.lsp.start({
  name = "pylsp",
  cmd = { "python3", "-m", "pylsp" },
  cmd_env = { PYTHONPATH = "/opt/pylsp", HOME = "/root" },
  root_dir = "/root/pyproj",
}, { bufnr = buf })
vim.wait(60000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local text = "no-reply"
-- `print(answer)`, line 1: the cursor is on `answer`, an int that jedi infers
-- from `answer = 42`; the hover is its docstring.
vim.wait(90000, function()
  local reply
  c:request("textDocument/hover", { textDocument = { uri = vim.uri_from_bufnr(buf) }, position = { line = 1, character = 8 } },
    function(_, r) reply = r end, buf)
  vim.wait(5000, function() return reply ~= nil end, 50)
  if reply and reply.contents then
    text = reply.contents.value or vim.inspect(reply.contents)
  end
  return text:find("integer", 1, true) ~= nil
end, 500)
out[#out + 1] = text:find("integer", 1, true) and "hover-int" or text
c:stop(true)
vim.wait(20000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/pylsp.txt", "w"):write(table.concat(out, ","), "\n")
