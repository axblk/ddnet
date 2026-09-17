//! The MCP server for DDNet maps.
//!
//! The tools live in C++ behind a C interface (`src/game/map/mcp/mcp_abi.h`);
//! this program speaks the protocol around them - over stdio for a client
//! that starts it, or over Streamable HTTP for anything that connects - with
//! the official Rust SDK, which knows both the per-request era of 2026-07-28
//! and the `initialize` handshake of the versions before it.

use std::ffi::{c_char, CStr, CString};
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use axum::http::{header, HeaderValue, StatusCode};
use axum::{extract::Request, middleware::Next, response::Response};
use rmcp::model::{
    CallToolRequestParams, CallToolResponse, CallToolResult, GetPromptRequestParams,
    GetPromptResponse, GetPromptResult, Implementation, ListPromptsResult,
    ListResourceTemplatesResult, ListResourcesResult, ListToolsResult, PaginatedRequestParams,
    ReadResourceRequestParams, ReadResourceResponse, ReadResourceResult, ServerCapabilities,
    ServerConfig, Tool,
};
use rmcp::service::RequestContext;
use rmcp::transport::streamable_http_server::session::local::LocalSessionManager;
use rmcp::transport::{stdio, StreamableHttpServerConfig, StreamableHttpService};
use rmcp::{ErrorData as McpError, RoleServer, ServerHandler, ServiceExt};
use serde_json::{json, Value};

// ---- the C interface ----

#[repr(C)]
struct RawServer {
    _private: [u8; 0],
}

extern "C" {
    fn ddnet_map_mcp_create(
        options_json: *const c_char,
        error_out: *mut *mut c_char,
    ) -> *mut RawServer;
    fn ddnet_map_mcp_destroy(server: *mut RawServer);
    fn ddnet_map_mcp_info(server: *mut RawServer) -> *mut c_char;
    fn ddnet_map_mcp_tools(server: *mut RawServer) -> *mut c_char;
    fn ddnet_map_mcp_call(
        server: *mut RawServer,
        name: *const c_char,
        arguments_json: *const c_char,
        meta_json: *const c_char,
    ) -> *mut c_char;
    fn ddnet_map_mcp_resources(server: *mut RawServer) -> *mut c_char;
    fn ddnet_map_mcp_resource_templates(server: *mut RawServer) -> *mut c_char;
    fn ddnet_map_mcp_read_resource(server: *mut RawServer, uri: *const c_char) -> *mut c_char;
    fn ddnet_map_mcp_prompts(server: *mut RawServer) -> *mut c_char;
    fn ddnet_map_mcp_get_prompt(
        server: *mut RawServer,
        name: *const c_char,
        arguments_json: *const c_char,
    ) -> *mut c_char;
    fn ddnet_map_mcp_close_idle(server: *mut RawServer);
    fn ddnet_map_mcp_free(text: *mut c_char);
}

/// The C++ server. Its calls may come from any thread: it queues them to
/// one worker of its own, so this is `Send` and `Sync` by construction.
struct Core(*mut RawServer);

unsafe impl Send for Core {}
unsafe impl Sync for Core {}

impl Drop for Core {
    fn drop(&mut self) {
        unsafe { ddnet_map_mcp_destroy(self.0) };
    }
}

/// Takes a string the C side handed out and frees it.
fn take(text: *mut c_char) -> String {
    if text.is_null() {
        return String::new();
    }
    let owned = unsafe { CStr::from_ptr(text) }
        .to_string_lossy()
        .into_owned();
    unsafe { ddnet_map_mcp_free(text) };
    owned
}

fn cstring(text: &str) -> CString {
    CString::new(text.replace('\0', "")).expect("no interior NUL after replacing it")
}

impl Core {
    fn create(options: &Value) -> Result<Core, String> {
        let options = cstring(&options.to_string());
        let mut error: *mut c_char = std::ptr::null_mut();
        let raw = unsafe { ddnet_map_mcp_create(options.as_ptr(), &mut error) };
        if raw.is_null() {
            return Err(take(error));
        }
        Ok(Core(raw))
    }

    fn json(text: String) -> Value {
        serde_json::from_str(&text).unwrap_or(Value::Null)
    }

    fn info(&self) -> Value {
        Self::json(take(unsafe { ddnet_map_mcp_info(self.0) }))
    }

    fn tools(&self) -> Value {
        Self::json(take(unsafe { ddnet_map_mcp_tools(self.0) }))
    }

    fn call(&self, name: &str, arguments: &Value, meta: &Value) -> Value {
        let name = cstring(name);
        let arguments = cstring(&arguments.to_string());
        let meta = cstring(&meta.to_string());
        Self::json(take(unsafe {
            ddnet_map_mcp_call(self.0, name.as_ptr(), arguments.as_ptr(), meta.as_ptr())
        }))
    }

    fn resources(&self) -> Value {
        Self::json(take(unsafe { ddnet_map_mcp_resources(self.0) }))
    }

    fn resource_templates(&self) -> Value {
        Self::json(take(unsafe { ddnet_map_mcp_resource_templates(self.0) }))
    }

    fn read_resource(&self, uri: &str) -> Value {
        let uri = cstring(uri);
        Self::json(take(unsafe {
            ddnet_map_mcp_read_resource(self.0, uri.as_ptr())
        }))
    }

    fn prompts(&self) -> Value {
        Self::json(take(unsafe { ddnet_map_mcp_prompts(self.0) }))
    }

    fn get_prompt(&self, name: &str, arguments: &Value) -> Value {
        let name = cstring(name);
        let arguments = cstring(&arguments.to_string());
        Self::json(take(unsafe {
            ddnet_map_mcp_get_prompt(self.0, name.as_ptr(), arguments.as_ptr())
        }))
    }

    fn close_idle(&self) {
        unsafe { ddnet_map_mcp_close_idle(self.0) };
    }
}

// ---- the protocol ----

#[derive(Clone)]
struct MapServer {
    core: Arc<Core>,
    name: String,
    version: String,
    instructions: String,
}

impl MapServer {
    fn new(core: Arc<Core>) -> MapServer {
        let info = core.info();
        MapServer {
            core,
            name: info["name"].as_str().unwrap_or("ddnet-map-mcp").to_string(),
            version: info["version"].as_str().unwrap_or("0").to_string(),
            instructions: info["instructions"].as_str().unwrap_or("").to_string(),
        }
    }

    /// Runs one call on the C side without holding the runtime's threads.
    async fn blocking<T: Send + 'static>(
        &self,
        job: impl FnOnce(&Core) -> T + Send + 'static,
    ) -> Result<T, McpError> {
        let core = self.core.clone();
        tokio::task::spawn_blocking(move || job(&core))
            .await
            .map_err(|error| {
                McpError::internal_error(format!("the tool call did not finish: {error}"), None)
            })
    }

    /// Turns `{"result": ...}` or `{"error": ...}` into the SDK's terms.
    fn unwrap_result(answer: Value) -> Result<Value, McpError> {
        if let Some(error) = answer.get("error") {
            let message = error["message"].as_str().unwrap_or("failed").to_string();
            return Err(match error["code"].as_i64() {
                Some(-32602) => McpError::invalid_params(message, None),
                Some(-32600) => McpError::invalid_request(message, None),
                _ => McpError::internal_error(message, None),
            });
        }
        answer.get("result").cloned().ok_or_else(|| {
            McpError::internal_error("the core answered with neither a result nor an error", None)
        })
    }

    fn parse<T: serde::de::DeserializeOwned>(value: Value) -> Result<T, McpError> {
        serde_json::from_value(value).map_err(|error| {
            McpError::internal_error(
                format!("the core's answer does not fit the protocol: {error}"),
                None,
            )
        })
    }
}

impl ServerHandler for MapServer {
    fn get_info(&self) -> ServerConfig {
        let mut config = ServerConfig::new(
            ServerCapabilities::builder()
                .enable_tools()
                .enable_resources()
                .enable_prompts()
                .build(),
        );
        let mut implementation = Implementation::from_build_env();
        implementation.name = self.name.clone();
        implementation.version = self.version.clone();
        implementation.title = Some("DDNet map tools".to_string());
        config.server_info = implementation;
        config.instructions = Some(self.instructions.clone());
        config
    }

    async fn list_tools(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListToolsResult, McpError> {
        let tools = self.blocking(|core| core.tools()).await?;
        let tools: Vec<Tool> = Self::parse(tools)?;
        Ok(ListToolsResult::with_all_items(tools))
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, McpError> {
        let name = request.name.to_string();
        let arguments = request
            .arguments
            .map(Value::Object)
            .unwrap_or_else(|| json!({}));
        let meta = request
            .meta
            .as_ref()
            .map(|meta| serde_json::to_value(meta).unwrap_or(Value::Null))
            .unwrap_or(Value::Null);
        let answer = self
            .blocking(move |core| core.call(&name, &arguments, &meta))
            .await?;
        let result: CallToolResult = Self::parse(Self::unwrap_result(answer)?)?;
        Ok(CallToolResponse::Complete(result))
    }

    async fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourcesResult, McpError> {
        let resources = self.blocking(|core| core.resources()).await?;
        Ok(ListResourcesResult::with_all_items(Self::parse(resources)?))
    }

    async fn list_resource_templates(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourceTemplatesResult, McpError> {
        let templates = self.blocking(|core| core.resource_templates()).await?;
        Ok(ListResourceTemplatesResult::with_all_items(Self::parse(
            templates,
        )?))
    }

    async fn read_resource(
        &self,
        request: ReadResourceRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<ReadResourceResponse, McpError> {
        let uri = request.uri.clone();
        let answer = self.blocking(move |core| core.read_resource(&uri)).await?;
        let result: ReadResourceResult = Self::parse(Self::unwrap_result(answer)?)?;
        Ok(ReadResourceResponse::Complete(result))
    }

    async fn list_prompts(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListPromptsResult, McpError> {
        let prompts = self.blocking(|core| core.prompts()).await?;
        Ok(ListPromptsResult::with_all_items(Self::parse(prompts)?))
    }

    async fn get_prompt(
        &self,
        request: GetPromptRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<GetPromptResponse, McpError> {
        let name = request.name.clone();
        let arguments = request
            .arguments
            .as_ref()
            .map(|arguments| {
                Value::Object(
                    arguments
                        .iter()
                        .map(|(key, value)| (key.clone(), value.clone()))
                        .collect(),
                )
            })
            .unwrap_or_else(|| json!({}));
        let answer = self
            .blocking(move |core| core.get_prompt(&name, &arguments))
            .await?;
        let result: GetPromptResult = Self::parse(Self::unwrap_result(answer)?)?;
        Ok(GetPromptResponse::Complete(result))
    }
}

// ---- the program ----

struct Options {
    root: Option<String>,
    http: Option<String>,
    token: Option<String>,
    allowed_origins: Vec<String>,
    allowed_hosts: Vec<String>,
    max_body: usize,
    history_mb: u64,
    max_maps: u64,
    idle_seconds: i64,
    sequential_handles: bool,
    render: bool,
    path_prefix: String,
    sse: bool,
}

fn usage() -> ! {
    eprintln!(
        "usage: ddnet-map-mcp --root DIR [--http [ADDR]] [--token TOKEN] [--allow-origin ORIGIN]... [--allow-host HOST]...\n\
         \x20              [--max-body BYTES] [--history-mb N] [--max-maps N] [--idle-seconds N] [--sequential-handles] [--no-render] [--path /mcp] [--sse]\n\
         \n\
         Serves the DDNet map tools over stdio (default) or Streamable HTTP (--http, default 127.0.0.1:8765).\n\
         Answers are plain JSON unless --sse asks for event streams.\n\
         Maps are read from and written to DIR only. DDNET_MAP_MCP_TOKEN may hold the bearer token instead of --token."
    );
    std::process::exit(2);
}

fn parse_options() -> Options {
    let mut options = Options {
        root: None,
        http: None,
        token: std::env::var("DDNET_MAP_MCP_TOKEN")
            .ok()
            .filter(|token| !token.is_empty()),
        allowed_origins: Vec::new(),
        allowed_hosts: Vec::new(),
        max_body: 4 * 1024 * 1024,
        history_mb: 256,
        max_maps: 4,
        idle_seconds: 0,
        sequential_handles: false,
        render: true,
        path_prefix: "/mcp".to_string(),
        sse: false,
    };
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut index = 0;
    while index < args.len() {
        let arg = args[index].clone();
        index += 1;
        let mut value = |name: &str| {
            let value = args.get(index).cloned().unwrap_or_else(|| {
                eprintln!("{name} needs a value");
                usage()
            });
            index += 1;
            value
        };
        match arg.as_str() {
            "--root" => options.root = Some(value("--root")),
            "--http" => {
                // An address may follow, or not.
                options.http = Some(match args.get(index) {
                    Some(next) if !next.starts_with("--") => {
                        index += 1;
                        next.clone()
                    }
                    _ => "127.0.0.1:8765".to_string(),
                });
            }
            "--token" => options.token = Some(value("--token")),
            "--allow-origin" => options.allowed_origins.push(value("--allow-origin")),
            "--allow-host" => options.allowed_hosts.push(value("--allow-host")),
            "--max-body" => {
                options.max_body = value("--max-body").parse().unwrap_or_else(|_| usage())
            }
            "--history-mb" => {
                options.history_mb = value("--history-mb").parse().unwrap_or_else(|_| usage())
            }
            "--max-maps" => {
                options.max_maps = value("--max-maps").parse().unwrap_or_else(|_| usage())
            }
            "--idle-seconds" => {
                options.idle_seconds = value("--idle-seconds").parse().unwrap_or_else(|_| usage())
            }
            "--sequential-handles" => options.sequential_handles = true,
            "--no-render" => options.render = false,
            "--path" => options.path_prefix = value("--path"),
            "--sse" => options.sse = true,
            "--stdio" => options.http = None,
            "-h" | "--help" => usage(),
            _ => {
                eprintln!("unknown argument '{arg}'");
                usage()
            }
        }
    }
    if options.root.is_none() {
        eprintln!("--root is required");
        usage();
    }
    options
}

/// Refuses anything without the bearer token, when one is set.
async fn require_token(token: Arc<Option<String>>, request: Request, next: Next) -> Response {
    if let Some(token) = token.as_ref() {
        let expected = format!("Bearer {token}");
        let given = request
            .headers()
            .get(header::AUTHORIZATION)
            .and_then(|value| value.to_str().ok());
        if given != Some(expected.as_str()) {
            return Response::builder()
                .status(StatusCode::UNAUTHORIZED)
                .header(header::WWW_AUTHENTICATE, HeaderValue::from_static("Bearer"))
                .body("a bearer token is required".into())
                .expect("a plain response");
        }
    }
    next.run(request).await
}

#[tokio::main]
async fn main() {
    let options = parse_options();
    let exe = std::env::current_exe()
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_else(|_| "ddnet-map-mcp".to_string());
    let core = Core::create(&json!({
        "root": options.root,
        "historyMb": options.history_mb,
        "maxMaps": options.max_maps,
        "idleSeconds": options.idle_seconds,
        "sequentialHandles": options.sequential_handles,
        "render": options.render,
        "args": [exe],
    }))
    .unwrap_or_else(|error| {
        eprintln!("ddnet-map-mcp: {error}");
        std::process::exit(1);
    });
    let core = Arc::new(core);
    if options.idle_seconds > 0 {
        let core = core.clone();
        tokio::spawn(async move {
            let mut ticker = tokio::time::interval(Duration::from_secs(30));
            loop {
                ticker.tick().await;
                let core = core.clone();
                let _ = tokio::task::spawn_blocking(move || core.close_idle()).await;
            }
        });
    }
    let handler = MapServer::new(core);

    match options.http {
        None => {
            let service = handler.serve(stdio()).await.unwrap_or_else(|error| {
                eprintln!("ddnet-map-mcp: stdio: {error}");
                std::process::exit(1);
            });
            if let Err(error) = service.waiting().await {
                eprintln!("ddnet-map-mcp: {error}");
            }
        }
        Some(address) => {
            let address: SocketAddr = address.parse().unwrap_or_else(|_| {
                eprintln!("'{address}' is not an address such as 127.0.0.1:8765");
                usage()
            });
            // Stateless in every era: 2026-07-28 has no sessions, and the
            // versions before it are served without one too, so that any
            // request may reach any process. Hosts are checked against the
            // loopback names unless told otherwise, and a browser origin is
            // refused unless it was allowed (by default only pages from this
            // very address) - a page on some site must not reach a server on
            // this machine.
            let cancel = tokio_util::sync::CancellationToken::new();
            let mut config = StreamableHttpServerConfig::default()
                .with_legacy_session_mode(false)
                .with_json_response(!options.sse)
                .with_max_request_body_bytes(options.max_body)
                .with_cancellation_token(cancel.clone());
            let allowed_origins = if options.allowed_origins.is_empty() {
                // A page served from this machine may talk to it; any other
                // page is refused.
                vec![
                    format!("http://localhost:{}", address.port()),
                    format!("http://127.0.0.1:{}", address.port()),
                    format!("http://[::1]:{}", address.port()),
                ]
            } else {
                options.allowed_origins.clone()
            };
            config = config
                .with_allowed_origins(allowed_origins)
                .enforce_origin_validation();
            if !options.allowed_hosts.is_empty() {
                config = config.with_allowed_hosts(options.allowed_hosts.clone());
            }
            let handler_for_service = handler.clone();
            let service = StreamableHttpService::new(
                move || Ok(handler_for_service.clone()),
                Arc::new(LocalSessionManager::default()),
                config,
            );
            let token = Arc::new(options.token.clone());
            let app = axum::Router::new()
                .nest_service(&options.path_prefix, service)
                .layer(axum::middleware::from_fn(move |request, next| {
                    require_token(token.clone(), request, next)
                }));
            let listener = tokio::net::TcpListener::bind(address)
                .await
                .unwrap_or_else(|error| {
                    eprintln!("ddnet-map-mcp: cannot listen on {address}: {error}");
                    std::process::exit(1);
                });
            let bound = listener
                .local_addr()
                .expect("a bound listener has an address");
            eprintln!(
                "ddnet-map-mcp: listening on http://{bound}{}",
                options.path_prefix
            );
            let serve = axum::serve(listener, app).with_graceful_shutdown(async move {
                let _ = tokio::signal::ctrl_c().await;
                cancel.cancel();
            });
            if let Err(error) = serve.await {
                eprintln!("ddnet-map-mcp: {error}");
            }
        }
    }
}
