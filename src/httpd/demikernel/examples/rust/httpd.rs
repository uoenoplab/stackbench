#![cfg_attr(feature = "strict", deny(warnings))]
#![deny(clippy::all)]
#![feature(extract_if)]
#![feature(hash_extract_if)]

//======================================================================================================================
// Imports
//======================================================================================================================

use anyhow::Result;
use ::clap::{
    Arg,
    ArgMatches,
    Command,
};
use demikernel::{
    demi_sgarray_t,
    runtime::types::{
        demi_opcode_t,
        demi_qresult_t,
    },
    LibOS,
    LibOSName,
    QDesc,
    QToken,
};
use std::{
    collections::{
        HashMap,
        HashSet,
    },
    str::FromStr,
    net::SocketAddrV4,
    slice,
    io::Write,
    cmp::min,
};

#[cfg(target_os = "windows")]
pub const AF_INET: i32 = windows::Win32::Networking::WinSock::AF_INET.0 as i32;

#[cfg(target_os = "windows")]
pub const SOCK_STREAM: i32 = windows::Win32::Networking::WinSock::SOCK_STREAM as i32;

#[cfg(target_os = "linux")]
pub const AF_INET: i32 = libc::AF_INET;

#[cfg(target_os = "linux")]
pub const SOCK_STREAM: i32 = libc::SOCK_STREAM;

//==============================================================================
// Program Arguments
//==============================================================================

/// Program Arguments
#[derive(Debug)]
pub struct ProgramArguments {
    /// Local socket IPv4 address.
    local: SocketAddrV4,
    len: u32,
}

/// Associate functions for Program Arguments
impl ProgramArguments {
    /// Default local socket IPv4 address.
    const DEFAULT_LOCAL: &'static str = "127.0.0.1:80";

    /// Parses the program arguments from the command line interface.
    pub fn new(app_name: &'static str, app_author: &'static str, app_about: &'static str) -> Result<Self> {
        let matches: ArgMatches = Command::new(app_name)
            .author(app_author)
            .about(app_about)
            .arg(
	        Arg::new("addr")
                    .long("address")
                    .value_parser(clap::value_parser!(String))
                    .required(true)
                    .value_name("ADDRESS:PORT")
                    .help("Sets socket address"),
            )
            .arg(
	        Arg::new("res_length")
                    .short('l')
                    .value_parser(clap::value_parser!(u32))
                    .required(true)
                    .value_name("BYTE")
                    .help("response length"),
            )
            .get_matches();

        // Default arguments.
        let mut args: ProgramArguments = ProgramArguments {
            local: SocketAddrV4::from_str(Self::DEFAULT_LOCAL)?,
	    len: 64,
        };

        // Local address.
        if let Some(addr) = matches.get_one::<String>("addr") {
            args.set_local_addr(addr)?;
        }

        if let Some(len) = matches.get_one::<u32>("res_length") {
	    args.len = *len;
        }

        Ok(args)
    }

    /// Returns the local endpoint address parameter stored in the target program arguments.
    pub fn get_local(&self) -> SocketAddrV4 {
        self.local
    }

    /// Sets the local address and port number parameters in the target program arguments.
    fn set_local_addr(&mut self, addr: &str) -> Result<()> {
        self.local = SocketAddrV4::from_str(addr)?;
        Ok(())
    }

    pub fn get_res_len(&self) -> u32 {
    	self.len
    }
}

//======================================================================================================================
// Structures
//======================================================================================================================

/// A TCP echo server.
pub struct TcpEchoServer {
    /// Underlying libOS.
    libos: LibOS,
    /// Local socket descriptor.
    sockqd: QDesc,
    /// Set of connected clients.
    clients: HashSet<QDesc>,
    /// List of pending operations.
    qts: Vec<QToken>,
    /// Reverse lookup table of pending operations.
    qts_reverse: HashMap<QToken, QDesc>,
    // HTTP Content
    contents: String,
}

//======================================================================================================================
// Associated Functions
//======================================================================================================================

impl TcpEchoServer {
    /// Instantiates a new TCP echo server.
    pub fn new(mut libos: LibOS, args: &ProgramArguments) -> Result<Self> {
        let local: SocketAddrV4 = args.get_local();

        // Create a TCP socket.
        let sockqd: QDesc = libos.socket(AF_INET, SOCK_STREAM, 0)?;

        // Bind the socket to a local address.
        if let Err(e) = libos.bind(sockqd, local) {
            println!("ERROR: {:?}", e);
            libos.close(sockqd)?;
            anyhow::bail!("{:?}", e);
        }

        // Enable the socket to accept incoming connections.
        if let Err(e) = libos.listen(sockqd, 16) {
            println!("ERROR: {:?}", e);
            libos.close(sockqd)?;
            anyhow::bail!("{:?}", e);
        }

        println!("INFO: listening on {:?}", local);

        return Ok(Self {
            libos,
            sockqd,
            clients: HashSet::default(),
            qts: Vec::default(),
            qts_reverse: HashMap::default(),
	    contents: (0..args.get_res_len()).map(|_| "a").collect::<String>()
        });
    }

    /// Runs the target TCP echo server.
    pub fn run(&mut self) -> Result<()> {
        // Accept first connection.
        {
            let qt: QToken = self.libos.accept(self.sockqd)?;
            let qr: demi_qresult_t = self.libos.wait(qt, None)?;
            if qr.qr_opcode != demi_opcode_t::DEMI_OPC_ACCEPT {
                anyhow::bail!("failed to accept connection")
            }
            self.handle_accept(&qr)?;
        }

        loop {
            // Wait for any operation to complete.
            let qr: demi_qresult_t = {
                let (index, qr): (usize, demi_qresult_t) = self.libos.wait_any(&self.qts, None)?;
                self.unregister_operation(index)?;
                qr
            };

            // Parse result.
            match qr.qr_opcode {
                demi_opcode_t::DEMI_OPC_ACCEPT => self.handle_accept(&qr)?,
                demi_opcode_t::DEMI_OPC_POP => self.handle_pop(&qr)?,
                demi_opcode_t::DEMI_OPC_PUSH => self.handle_push()?,
                demi_opcode_t::DEMI_OPC_FAILED => self.handle_fail(&qr)?,
                demi_opcode_t::DEMI_OPC_INVALID => self.handle_unexpected("invalid", &qr)?,
                demi_opcode_t::DEMI_OPC_CLOSE => self.handle_unexpected("close", &qr)?,
                demi_opcode_t::DEMI_OPC_CONNECT => self.handle_unexpected("connect", &qr)?,
            }
        }

        Ok(())
    }

    /// Issues an accept operation.
    fn issue_accept(&mut self) -> Result<()> {
        let qt: QToken = self.libos.accept(self.sockqd)?;
        self.register_operation(self.sockqd, qt);
        Ok(())
    }

    /// Issues a push operation.
    fn issue_push(&mut self, qd: QDesc, sga: &demi_sgarray_t) -> Result<()> {
        let qt: QToken = self.libos.push(qd, &sga)?;
        self.register_operation(qd, qt);
        Ok(())
    }

    /// Issues a pop operation.
    fn issue_pop(&mut self, qd: QDesc) -> Result<()> {
        let qt: QToken = self.libos.pop(qd, None)?;
        self.register_operation(qd, qt);
        Ok(())
    }

    /// Handles an operation that failed.
    fn handle_fail(&mut self, qr: &demi_qresult_t) -> Result<()> {
        let qd: QDesc = qr.qr_qd.into();
        let qt: QToken = qr.qr_qt.into();
        let errno: i64 = qr.qr_ret;

        // Check if client has reset the connection.
        if errno == libc::ECONNRESET as i64 || errno == libc::ECANCELED as i64 {
            if errno == libc::ECONNRESET as i64 {
                println!("INFO: client reset connection (qd={:?})", qd);
            } else {
                println!(
                    "INFO: operation cancelled, resetting connection (qd={:?}, qt={:?})",
                    qd, qt
                );
            }
            self.handle_close(qd)?;
        } else {
            println!(
                "WARN: operation failed, ignoring (qd={:?}, qt={:?}, errno={:?})",
                qd, qt, errno
            );
        }

        Ok(())
    }

    /// Handles the completion of a push operation.
    fn handle_push(&mut self) -> Result<()> {
        Ok(())
    }

    /// Handles the completion of an unexpected operation.
    fn handle_unexpected(&mut self, op_name: &str, qr: &demi_qresult_t) -> Result<()> {
        let qd: QDesc = qr.qr_qd.into();
        let qt: QToken = qr.qr_qt.into();
        println!(
            "WARN: unexpected {} operation completed, ignoring (qd={:?}, qt={:?})",
            op_name, qd, qt
        );
        Ok(())
    }

    /// Handles the completion of an accept operation.
    fn handle_accept(&mut self, qr: &demi_qresult_t) -> Result<()> {
        let new_qd: QDesc = unsafe { qr.qr_value.ares.qd.into() };

        // Register client.
        self.clients.insert(new_qd);

        // Pop first packet.
        self.issue_pop(new_qd)?;

        // Accept more connections.
        self.issue_accept()?;

        Ok(())
    }

    fn generate_response(&mut self, qd: QDesc) -> Result<Vec<demi_sgarray_t>> {
        const DATA_MAX: usize = 1464;

        let response = format!(
       	    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n",
            self.contents.len(),
        );

	let mut offset = 0;
	let mut vec = Vec::with_capacity((response.len() + self.contents.len() + DATA_MAX - 1) / DATA_MAX);

	loop {
	     if offset == response.len() + self.contents.len() {
	     	break;
	     }

	     let size = min(response.len() + self.contents.len() - offset, DATA_MAX);
	     
	     // Allocate scatter-gather array.
             let sga: demi_sgarray_t = match self.libos.sgaalloc(size) {
             	 Ok(sga) => sga,
            	 Err(e) => anyhow::bail!("failed to allocate scatter-gather array: {:?}", e),
             };

             // Ensure that scatter-gather array has the requested size.
             assert!(sga.sga_segs[0].sgaseg_len as usize == size);

             // prepare scatter-gather array.
             let ptr: *mut u8 = sga.sga_segs[0].sgaseg_buf as *mut u8;
             let len: usize = sga.sga_segs[0].sgaseg_len as usize;
             let mut slice: &mut [u8] = unsafe { slice::from_raw_parts_mut(ptr, len) };


	     if offset == 0 {
                 slice.write(&response.as_bytes())?;

        	 let mut slice2: &mut [u8] = unsafe { slice::from_raw_parts_mut(ptr.wrapping_add(response.len()), len - response.len()) };
                 slice2.write(&self.contents.as_bytes()[..size-response.len()])?;
	     } else {
                 slice.write(&self.contents.as_bytes()[offset - response.len()..offset + size - response.len()])?;
	     }
	     //vec.push(sga);
	     let qt: QToken = self.libos.push(qd, &sga)?;
	     self.libos.wait(qt, None)?;
	     self.libos.sgafree(sga)?;

	     offset += size;
	}
	

	Ok(vec)
    }


    /// Handles the completion of a pop() operation.
    fn handle_pop(&mut self, qr: &demi_qresult_t) -> Result<()> {
        let qd: QDesc = qr.qr_qd.into();
        let sga: demi_sgarray_t = unsafe { qr.qr_value.sga };

        // Check if we received any data.
        if sga.sga_segs[0].sgaseg_len == 0 {
            println!("INFO: client closed connection (qd={:?})", qd);
            self.handle_close(qd)?;
        } else {
	    let sga_vec = self.generate_response(qd)?;

            // Push packet back.
	    //for sga2 in sga_vec {
	    //	self.issue_push(qd, &sga2)?;
		//self.libos.sgafree(sga2)?;
	    //}

            // Pop more data.
            self.issue_pop(qd)?;
        }

        // Free scatter-gather array.
        self.libos.sgafree(sga)?;

        Ok(())
    }

    /// Handles a close operation.
    fn handle_close(&mut self, qd: QDesc) -> Result<()> {
        let qts_drained: HashMap<QToken, QDesc> = self.qts_reverse.extract_if(|_k, v| v == &qd).collect();
        let _: Vec<_> = self.qts.extract_if(|x| qts_drained.contains_key(x)).collect();
        self.clients.remove(&qd);
        self.libos.close(qd)?;
        Ok(())
    }

    /// Registers an asynchronous I/O operation.
    fn register_operation(&mut self, qd: QDesc, qt: QToken) {
        self.qts_reverse.insert(qt, qd);
        self.qts.push(qt);
    }

    /// Unregisters an asynchronous I/O operation.
    fn unregister_operation(&mut self, index: usize) -> Result<()> {
        let qt: QToken = self.qts.remove(index);
        self.qts_reverse
            .remove(&qt)
            .ok_or(anyhow::anyhow!("unregistered queue token"))?;
        Ok(())
    }
}

//======================================================================================================================
// Trait Implementations
//======================================================================================================================

impl Drop for TcpEchoServer {
    fn drop(&mut self) {
        // Close local socket and cancel all pending operations.
        for qd in self.clients.drain().collect::<Vec<_>>() {
            if let Err(e) = self.handle_close(qd) {
                println!("ERROR: close() failed (error={:?}", e);
                println!("WARN: leaking qd={:?}", qd);
            }
        }
        if let Err(e) = self.handle_close(self.sockqd) {
            println!("ERROR: {:?}", e);
        }
    }
}

fn main() -> Result<()> {
    let args: ProgramArguments = ProgramArguments::new(
        "",
        "",
        "",
    )?;

    let libos_name: LibOSName = match LibOSName::from_env() {
        Ok(libos_name) => libos_name.into(),
        Err(e) => anyhow::bail!("{:?}", e),
    };
    let libos: LibOS = match LibOS::new(libos_name) {
        Ok(libos) => libos,
        Err(e) => anyhow::bail!("failed to initialize libos: {:?}", e.cause),
    };

    TcpEchoServer::new(libos, &args)?.run()
}
