-- A minimal language server for the Alpine lane: it speaks LSP over
-- standard input and output, run as `nvim -l server.lua`. It answers
-- initialize with a hover provider, hover with a fixed text, and
-- shutdown, and it exits on `exit`.
local function send(msg)
  local body = vim.json.encode(msg)
  io.stdout:write("Content-Length: ", #body, "\r\n\r\n", body)
  io.stdout:flush()
end

while true do
  local len
  while true do
    local line = io.stdin:read("*l")
    if not line then os.exit(0) end
    line = line:gsub("\r$", "")
    if line == "" then break end
    len = tonumber(line:match("^Content%-Length: (%d+)")) or len
  end
  local msg = vim.json.decode(io.stdin:read(len))
  if msg.method == "initialize" then
    send({ jsonrpc = "2.0", id = msg.id, result = { capabilities = { hoverProvider = true } } })
  elseif msg.method == "textDocument/hover" then
    send({ jsonrpc = "2.0", id = msg.id, result = { contents = { kind = "plaintext", value = "hover from the stub" } } })
  elseif msg.method == "shutdown" then
    send({ jsonrpc = "2.0", id = msg.id, result = vim.NIL })
  elseif msg.method == "exit" then
    os.exit(0)
  end
end
