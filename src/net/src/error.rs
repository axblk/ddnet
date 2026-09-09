use std::error;
use std::fmt;
use std::result;

#[derive(Debug)]
pub struct Error {
    message: String,
    fatal: bool,
}

impl Error {
    pub fn from_string(string: String) -> Error {
        Error {
            message: string,
            fatal: false,
        }
    }
    /// Marks an error the network object cannot recover from: the socket or
    /// the poll behind it is gone. Everything else concerns one peer or one
    /// call and leaves the object usable.
    pub fn fatal(mut self) -> Error {
        self.fatal = true;
        self
    }
    pub fn is_fatal(&self) -> bool {
        self.fatal
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.message.fmt(f)
    }
}
impl error::Error for Error {}

pub type Result<T> = result::Result<T, Error>;

pub trait Context {
    type T;
    fn context(self, context: &str) -> Result<Self::T>;
}

impl<T, E: fmt::Display> Context for result::Result<T, E> {
    type T = T;
    fn context(self, context: &str) -> Result<T> {
        self.map_err(|e| Error::from_string(format!("{}: {}", context, e)))
    }
}
