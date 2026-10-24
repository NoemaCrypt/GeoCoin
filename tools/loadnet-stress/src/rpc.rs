use std::error::Error;
use std::fmt::{Display, Formatter};
use std::io::{Read, Write};
use std::net::{Shutdown, TcpStream};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use reqwest::blocking::Client;
use reqwest::header::CONTENT_TYPE;
use serde::de::DeserializeOwned;
use serde_json::{Value, json};

const MAX_IN_FLIGHT_PER_RPC_CLIENT: usize = 3;
const MAX_RAW_HTTP_RESPONSE_BYTES: usize = 2 * 1024 * 1024;

struct CallLimiter {
    in_flight: Mutex<usize>,
    ready: Condvar,
}

struct CallPermit {
    limiter: Arc<CallLimiter>,
}

impl CallLimiter {
    fn acquire(self: &Arc<Self>) -> CallPermit {
        let mut in_flight = self.in_flight.lock().expect("RPC limiter mutex poisoned");
        while *in_flight >= MAX_IN_FLIGHT_PER_RPC_CLIENT {
            in_flight = self
                .ready
                .wait(in_flight)
                .expect("RPC limiter mutex poisoned");
        }
        *in_flight += 1;
        CallPermit {
            limiter: Arc::clone(self),
        }
    }
}

impl Drop for CallPermit {
    fn drop(&mut self) {
        let mut in_flight = self
            .limiter
            .in_flight
            .lock()
            .expect("RPC limiter mutex poisoned");
        *in_flight = in_flight.saturating_sub(1);
        self.limiter.ready.notify_one();
    }
}

#[derive(Clone)]
pub struct RpcClient {
    url: String,
    user: String,
    password: String,
    http: Client,
    limiter: Arc<CallLimiter>,
}

#[derive(Debug, Clone)]
pub enum RpcError {
    Transport(String),
    InvalidResponse(String),
    Remote { code: i64, message: String },
}

impl RpcError {
    pub fn remote_code(&self) -> Option<i64> {
        match self {
            Self::Remote { code, .. } => Some(*code),
            _ => None,
        }
    }

    pub fn remote_message(&self) -> Option<&str> {
        match self {
            Self::Remote { message, .. } => Some(message),
            _ => None,
        }
    }

    pub fn is_remote(&self) -> bool {
        matches!(self, Self::Remote { .. })
    }
}

#[derive(Debug, Clone)]
pub struct RawHttpResponse {
    pub status: u16,
    pub headers: String,
    pub body: String,
}

pub(crate) fn basic_auth_base64(user: &str, password: &str) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let input = format!("{user}:{password}");
    let bytes = input.as_bytes();
    let mut encoded = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let first = chunk[0];
        let second = chunk.get(1).copied().unwrap_or(0);
        let third = chunk.get(2).copied().unwrap_or(0);
        encoded.push(ALPHABET[(first >> 2) as usize] as char);
        encoded.push(ALPHABET[(((first & 0x03) << 4) | (second >> 4)) as usize] as char);
        encoded.push(if chunk.len() > 1 {
            ALPHABET[(((second & 0x0f) << 2) | (third >> 6)) as usize] as char
        } else {
            '='
        });
        encoded.push(if chunk.len() > 2 {
            ALPHABET[(third & 0x3f) as usize] as char
        } else {
            '='
        });
    }
    encoded
}

fn parse_raw_http_response(raw: &[u8]) -> Result<RawHttpResponse, RpcError> {
    let header_end = raw
        .windows(4)
        .position(|window| window == b"\r\n\r\n")
        .map(|position| position + 4)
        .or_else(|| {
            raw.windows(2)
                .position(|window| window == b"\n\n")
                .map(|position| position + 2)
        })
        .ok_or_else(|| {
            RpcError::InvalidResponse("raw HTTP response has no header terminator".to_owned())
        })?;
    let headers = String::from_utf8_lossy(&raw[..header_end]).into_owned();
    let status = headers
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .and_then(|value| value.parse::<u16>().ok())
        .ok_or_else(|| {
            RpcError::InvalidResponse("raw HTTP response has no valid status".to_owned())
        })?;
    Ok(RawHttpResponse {
        status,
        headers,
        body: String::from_utf8_lossy(&raw[header_end..]).into_owned(),
    })
}

impl Display for RpcError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Transport(message) => write!(f, "RPC transport error: {message}"),
            Self::InvalidResponse(message) => write!(f, "invalid RPC response: {message}"),
            Self::Remote { code, message } => write!(f, "RPC error {code}: {message}"),
        }
    }
}

impl Error for RpcError {}

impl RpcClient {
    pub fn new(
        url: String,
        user: String,
        password: String,
        timeout: Duration,
    ) -> Result<Self, RpcError> {
        let http = Client::builder()
            .connect_timeout(Duration::from_secs(5))
            .timeout(timeout)
            .pool_max_idle_per_host(8)
            .build()
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        Ok(Self {
            url,
            user,
            password,
            http,
            limiter: Arc::new(CallLimiter {
                in_flight: Mutex::new(0),
                ready: Condvar::new(),
            }),
        })
    }

    pub fn call(&self, method: &str, params: Value) -> Result<Value, RpcError> {
        // The node deliberately limits simultaneous loopback clients. Queue
        // excess worker requests here so a stress run measures transaction
        // processing instead of losing requests during TCP admission bursts.
        let _permit = self.limiter.acquire();
        let body = json!({
            "jsonrpc": "2.0",
            "id": method,
            "method": method,
            "params": params,
        });
        let mut last_transport_error = String::new();
        let mut accepted_response = None;
        for attempt in 0_u64..=7 {
            match self
                .http
                .post(&self.url)
                .basic_auth(&self.user, Some(&self.password))
                .json(&body)
                .send()
            {
                Ok(response)
                    if response.status().as_u16() != 429 && response.status().as_u16() != 503 =>
                {
                    accepted_response = Some(response);
                    break;
                }
                Ok(response) => {
                    last_transport_error = format!("HTTP {}", response.status());
                }
                Err(error) => {
                    last_transport_error = error.to_string();
                }
            }
            // Local loadnet RPC deliberately has a four-connections-per-client
            // cap. Short bounded retries provide backpressure while preserving
            // real concurrency across the four nodes.
            if attempt < 7 {
                std::thread::sleep(Duration::from_millis(10_u64 << attempt.min(5)));
            }
        }
        let response = accepted_response.ok_or(RpcError::Transport(last_transport_error))?;
        let status = response.status();
        let payload: Value = response
            .json()
            .map_err(|error| RpcError::InvalidResponse(format!("HTTP {status}: {error}")))?;

        if let Some(error) = payload.get("error").filter(|value| !value.is_null()) {
            return Err(RpcError::Remote {
                code: error.get("code").and_then(Value::as_i64).unwrap_or(-1),
                message: error
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown RPC failure")
                    .to_owned(),
            });
        }
        payload.get("result").cloned().ok_or_else(|| {
            RpcError::InvalidResponse(format!("HTTP {status}: missing result field"))
        })
    }

    /// Send one deliberately unprocessed JSON body. Security scenarios use
    /// this to verify HTTP authentication and strict JSON-RPC parsing instead
    /// of going through the normal typed request builder.
    pub fn raw_json_post(
        &self,
        body: &str,
        authenticate: bool,
    ) -> Result<RawHttpResponse, RpcError> {
        let _permit = self.limiter.acquire();
        let mut request = self
            .http
            .post(&self.url)
            .header(CONTENT_TYPE, "application/json")
            .body(body.to_owned());
        if authenticate {
            request = request.basic_auth(&self.user, Some(&self.password));
        }
        let response = request
            .send()
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        let status = response.status().as_u16();
        let headers = response
            .headers()
            .iter()
            .map(|(name, value)| {
                format!(
                    "{}: {}",
                    name.as_str(),
                    value.to_str().unwrap_or("<binary>")
                )
            })
            .collect::<Vec<_>>()
            .join("\r\n");
        let body = response
            .text()
            .map_err(|error| RpcError::InvalidResponse(error.to_string()))?;
        Ok(RawHttpResponse {
            status,
            headers,
            body,
        })
    }

    /// Send a deliberately malformed or boundary HTTP/1.1 request directly
    /// over TCP. Unlike reqwest, this permits duplicate/conflicting framing
    /// headers so the live node's parser can be tested end to end.
    pub fn raw_http_request(
        &self,
        request_line: &str,
        headers: &[(&str, &str)],
        body: &[u8],
        authenticate: bool,
    ) -> Result<RawHttpResponse, RpcError> {
        let _permit = self.limiter.acquire();
        let url = reqwest::Url::parse(&self.url)
            .map_err(|error| RpcError::Transport(format!("invalid RPC URL: {error}")))?;
        if url.scheme() != "http" {
            return Err(RpcError::Transport(
                "raw HTTP security probe only supports http:// URLs".to_owned(),
            ));
        }
        let host = url
            .host_str()
            .ok_or_else(|| RpcError::Transport("RPC URL has no host".to_owned()))?;
        let port = url
            .port_or_known_default()
            .ok_or_else(|| RpcError::Transport("RPC URL has no port".to_owned()))?;
        let default_host = if host.contains(':') {
            format!("[{host}]:{port}")
        } else {
            format!("{host}:{port}")
        };

        let has_header = |wanted: &str| {
            headers
                .iter()
                .any(|(name, _)| name.eq_ignore_ascii_case(wanted))
        };
        let mut request = Vec::new();
        write!(&mut request, "{request_line}\r\n")
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        if !has_header("Host") {
            write!(&mut request, "Host: {default_host}\r\n")
                .map_err(|error| RpcError::Transport(error.to_string()))?;
        }
        if authenticate && !has_header("Authorization") {
            write!(
                &mut request,
                "Authorization: Basic {}\r\n",
                basic_auth_base64(&self.user, &self.password)
            )
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        }
        for (name, value) in headers {
            write!(&mut request, "{name}: {value}\r\n")
                .map_err(|error| RpcError::Transport(error.to_string()))?;
        }
        if !has_header("Content-Length") && !has_header("Transfer-Encoding") {
            write!(&mut request, "Content-Length: {}\r\n", body.len())
                .map_err(|error| RpcError::Transport(error.to_string()))?;
        }
        if !has_header("Connection") {
            request.extend_from_slice(b"Connection: close\r\n");
        }
        request.extend_from_slice(b"\r\n");
        request.extend_from_slice(body);

        let mut stream = TcpStream::connect((host, port))
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        stream
            .set_write_timeout(Some(Duration::from_secs(5)))
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        stream
            .write_all(&request)
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        stream
            .shutdown(Shutdown::Write)
            .map_err(|error| RpcError::Transport(error.to_string()))?;

        let mut raw = Vec::new();
        stream
            .take((MAX_RAW_HTTP_RESPONSE_BYTES + 1) as u64)
            .read_to_end(&mut raw)
            .map_err(|error| RpcError::Transport(error.to_string()))?;
        if raw.len() > MAX_RAW_HTTP_RESPONSE_BYTES {
            return Err(RpcError::InvalidResponse(format!(
                "raw HTTP response exceeds {MAX_RAW_HTTP_RESPONSE_BYTES} bytes"
            )));
        }
        parse_raw_http_response(&raw)
    }

    pub fn call_as<T: DeserializeOwned>(&self, method: &str, params: Value) -> Result<T, RpcError> {
        let value = self.call(method, params)?;
        serde_json::from_value(value)
            .map_err(|error| RpcError::InvalidResponse(format!("{method}: {error}")))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::thread;

    #[test]
    fn call_limiter_caps_parallel_requests() {
        let limiter = Arc::new(CallLimiter {
            in_flight: Mutex::new(0),
            ready: Condvar::new(),
        });
        let active = Arc::new(AtomicUsize::new(0));
        let peak = Arc::new(AtomicUsize::new(0));
        let mut handles = Vec::new();
        for _ in 0..12 {
            let limiter = Arc::clone(&limiter);
            let active = Arc::clone(&active);
            let peak = Arc::clone(&peak);
            handles.push(thread::spawn(move || {
                let _permit = limiter.acquire();
                let now = active.fetch_add(1, Ordering::SeqCst) + 1;
                peak.fetch_max(now, Ordering::SeqCst);
                thread::sleep(Duration::from_millis(5));
                active.fetch_sub(1, Ordering::SeqCst);
            }));
        }
        for handle in handles {
            handle.join().expect("limiter test thread panicked");
        }
        assert_eq!(peak.load(Ordering::SeqCst), MAX_IN_FLIGHT_PER_RPC_CLIENT);
    }

    #[test]
    fn basic_auth_and_raw_http_response_parser_are_canonical() {
        assert_eq!(basic_auth_base64("user", "pass"), "dXNlcjpwYXNz");
        assert_eq!(basic_auth_base64("a", ""), "YTo=");
        let response = parse_raw_http_response(
            b"HTTP/1.1 413 Payload Too Large\r\nContent-Length: 4\r\n\r\nnope",
        )
        .expect("raw response parses");
        assert_eq!(response.status, 413);
        assert!(response.headers.contains("Content-Length: 4"));
        assert_eq!(response.body, "nope");
    }
}
