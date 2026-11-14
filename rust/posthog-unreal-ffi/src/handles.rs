//! Client admission, acquisition ownership and tracked shutdown. No SDK policy lives here.
use std::collections::HashMap;
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use posthog_rs::stateful::{Client, DrainReport, FeatureFlagsSubscription, ReplayFrame};

use crate::abi::{Result, Status};
use crate::flags::Mailbox;
use crate::hooks::CallbackScope;

pub struct Entry {
    pub client: Arc<Client>,
    pub mailbox: Arc<Mailbox>,
    pub scope: Arc<CallbackScope>,
    pub subscription: Mutex<Option<FeatureFlagsSubscription>>,
    pub timeout: Duration,
    calls: Mutex<Calls>,
    idle: Condvar,
}
#[derive(Default)]
struct Calls {
    active: usize,
    close: Option<Closing>,
}
struct Closing {
    deadline: Instant,
    worker: Option<JoinHandle<DrainReport>>,
    joining: bool,
    report: Option<Result<DrainReport>>,
}
pub struct Frame {
    pub owner: u64,
    pub frame: ReplayFrame,
}
#[derive(Default)]
pub struct Registry {
    next: u64,
    pub clients: HashMap<u64, Arc<Entry>>,
    pub frames: HashMap<u64, Frame>,
}
impl Registry {
    pub fn next(&mut self) -> Result<u64> {
        self.next = self.next.checked_add(1).ok_or(Status::InternalError)?;
        Ok(self.next)
    }
}
pub fn registry() -> &'static Mutex<Registry> {
    static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(Registry::default()))
}
pub fn lookup(handle: u64) -> Result<Arc<Entry>> {
    registry()
        .lock()
        .unwrap_or_else(|p| p.into_inner())
        .clients
        .get(&handle)
        .cloned()
        .ok_or(Status::Closed)
}
pub struct Call(pub Arc<Entry>);
impl Drop for Call {
    fn drop(&mut self) {
        let mut calls = self.0.calls.lock().unwrap_or_else(|p| p.into_inner());
        calls.active -= 1;
        self.0.idle.notify_all();
    }
}
pub fn acquire(handle: u64) -> Result<Call> {
    let entry = lookup(handle)?;
    {
        let mut calls = entry.calls.lock().unwrap_or_else(|p| p.into_inner());
        if calls.close.is_some() {
            return Err(Status::Closed);
        }
        calls.active += 1;
    }
    Ok(Call(entry))
}
impl Entry {
    pub fn new(
        client: Client,
        mailbox: Arc<Mailbox>,
        scope: Arc<CallbackScope>,
        timeout: Duration,
    ) -> Self {
        let inbox = mailbox.clone();
        let subscription = client.on_feature_flags(move |update| inbox.push(update));
        Self {
            client: Arc::new(client),
            mailbox,
            scope,
            subscription: Mutex::new(Some(subscription)),
            timeout,
            calls: Mutex::new(Calls::default()),
            idle: Condvar::new(),
        }
    }
    pub fn begin(&self) -> Result<()> {
        let spawn_ok = {
            let mut calls = self.calls.lock().unwrap_or_else(|p| p.into_inner());
            if calls.close.is_some() {
                return Ok(());
            }
            let deadline = Instant::now() + self.timeout;
            let client = self.client.clone();
            let worker = thread::Builder::new()
                .name("posthog-ffi-close".into())
                .spawn(move || {
                    client
                        .shutdown_blocking(Some(deadline.saturating_duration_since(Instant::now())))
                });
            let ok = worker.is_ok();
            calls.close = Some(Closing {
                deadline,
                worker: worker.ok(),
                joining: false,
                report: None,
            });
            ok
        };
        // SDK unsubscribe may destroy captures. Never do that under an admission/registry lock.
        let subscription = self
            .subscription
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .take();
        if let Some(subscription) = subscription {
            subscription.unsubscribe();
        }
        self.mailbox.close();
        if spawn_ok {
            Ok(())
        } else {
            Err(Status::InternalError)
        }
    }
    pub fn finish(&self) -> Result<DrainReport> {
        if self.scope.active() {
            return Err(Status::Deferred);
        }
        let (worker, deadline) = {
            let mut calls = self.calls.lock().unwrap_or_else(|p| p.into_inner());
            loop {
                let close = calls.close.as_ref().ok_or(Status::InternalError)?;
                if let Some(report) = close.report {
                    return report;
                }
                if calls.active == 0 && !close.joining {
                    break;
                }
                calls = self.idle.wait(calls).unwrap_or_else(|p| p.into_inner());
            }
            let close = calls.close.as_mut().unwrap();
            close.joining = true;
            (close.worker.take(), close.deadline)
        };
        let report = match worker {
            Some(worker) => worker.join().map_err(|_| Status::InternalError),
            // If thread creation failed, finish still provides synchronous cleanup.
            None => std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                self.client
                    .shutdown_blocking(Some(deadline.saturating_duration_since(Instant::now())))
            }))
            .map_err(|_| Status::InternalError),
        };
        let mut calls = self.calls.lock().unwrap_or_else(|p| p.into_inner());
        let close = calls.close.as_mut().unwrap();
        close.joining = false;
        close.report = Some(report);
        self.idle.notify_all();
        report
    }
    pub fn insert_frame(&self, owner: u64, frame: ReplayFrame) -> Result<u64> {
        let calls = self.calls.lock().unwrap_or_else(|p| p.into_inner());
        if calls.close.is_some() {
            return Err(Status::Closed);
        }
        let mut registry = registry().lock().unwrap_or_else(|p| p.into_inner());
        let id = registry.next()?;
        registry.frames.insert(id, Frame { owner, frame });
        Ok(id)
    }
}
pub fn begin(handle: u64) -> Result<Arc<Entry>> {
    let entry = lookup(handle)?;
    let result = entry.begin();
    // Closing admission and insertion use the same entry lock. Remove all host-held leases.
    let frames = {
        let mut registry = registry().lock().unwrap_or_else(|p| p.into_inner());
        let ids: Vec<_> = registry
            .frames
            .iter()
            .filter_map(|(id, frame)| (frame.owner == handle).then_some(*id))
            .collect();
        ids.into_iter()
            .filter_map(|id| registry.frames.remove(&id))
            .collect::<Vec<_>>()
    };
    drop(frames);
    result.map(|()| entry)
}
pub fn take_frame(frame: u64) -> Result<Frame> {
    registry()
        .lock()
        .unwrap_or_else(|p| p.into_inner())
        .frames
        .remove(&frame)
        .ok_or(Status::Closed)
}
