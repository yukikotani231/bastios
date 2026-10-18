-- Starts Eclipse JDT LS (the JDK and jdtls unpacked under /opt in the Alpine
-- root) with Neovim's LSP client on the project in /root/javaproj, asks for
-- the hover on a call, stops the client, and writes what happened to
-- /root/jdtls.txt.
local out = {}
vim.cmd.edit("/root/javaproj/Main.java")
local buf = vim.api.nvim_get_current_buf()
local launcher = vim.fn.glob("/opt/jdtls/plugins/org.eclipse.equinox.launcher_*.jar")
local id = vim.lsp.start({
  name = "jdtls",
  cmd = { "/opt/java/openjdk/bin/java",
    "-Declipse.application=org.eclipse.jdt.ls.core.id1", "-Dosgi.bundles.defaultStartLevel=4",
    "-Declipse.product=org.eclipse.jdt.ls.core.product", "-Dlog.level=ERROR", "-Xmx512m",
    "--add-modules=ALL-SYSTEM", "--add-opens", "java.base/java.util=ALL-UNNAMED",
    "--add-opens", "java.base/java.lang=ALL-UNNAMED",
    "-jar", launcher, "-configuration", "/opt/jdtls/config_linux", "-data", "/tmp/jdtls-data" },
  cmd_env = { JAVA_HOME = "/opt/java/openjdk", HOME = "/root" },
  root_dir = "/root/javaproj",
}, { bufnr = buf })
vim.wait(900000, function()
  local c = vim.lsp.get_client_by_id(id)
  return c ~= nil and c.initialized
end, 100)
local c = vim.lsp.get_client_by_id(id)
out[#out + 1] = (c and c.initialized) and "initialized" or "no-init"
local text = "no-reply"
-- `System.out.println(add(1, 2))`, line 4: the cursor is on `add`; the hover is its signature.
vim.wait(900000, function()
  local reply
  c:request("textDocument/hover", { textDocument = { uri = vim.uri_from_bufnr(buf) }, position = { line = 4, character = 28 } },
    function(_, r) reply = r end, buf)
  vim.wait(5000, function() return reply ~= nil end, 50)
  if reply and reply.contents then
    text = vim.inspect(reply.contents)
  end
  return text:find("add(int a, int b)", 1, true) ~= nil
end, 2000)
out[#out + 1] = text:find("add(int a, int b)", 1, true) and "hover-add" or text
c:stop(true)
vim.wait(60000, function() return vim.lsp.get_client_by_id(id) == nil end, 50)
out[#out + 1] = vim.lsp.get_client_by_id(id) == nil and "stopped" or "running"
io.open("/root/jdtls.txt", "w"):write(table.concat(out, ","), "\n")
