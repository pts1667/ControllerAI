--
-- ControllerAI Options
--

local options = {
	{
		key    = 'server_section',
		name   = 'External Server Settings',
		desc   = 'Configure the binding address and port for the external AI communication layer.',
		type   = 'section',
	},
	{
		key     = 'ip',
		name    = 'Binding Address',
		desc    = 'The IP address the HTTP server will bind to. Use 0.0.0.0 for all interfaces or 127.0.0.1 for local only.',
		type    = 'string',
		section = 'server_section',
		def     = '127.0.0.1',
	},
	{
		key     = 'port',
		name    = 'Server Port',
		desc    = 'The port the HTTP/WebSocket server will listen on.',
		type    = 'number',
		section = 'server_section',
		def     = 3017,
		min     = 1024,
		max     = 65535,
		step    = 1.0,
	},
	{
		key     = 'sync',
		name    = 'Synchronous Mode',
		desc    = 'If enabled, the game will pause at the end of every frame until a "finish_frame" command is received.',
		type    = 'bool',
		section = 'server_section',
		def     = true,
	},
}

return options
