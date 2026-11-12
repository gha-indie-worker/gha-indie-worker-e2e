use std::collections::HashMap;

const MAX_RETRY_AFTER_MS: u64 = 10_000;

#[derive(Debug)]
struct Bucket {
    capacity: f64,
    refill_per_second: f64,
    tokens: HashMap<String, f64>,
    clock_ms: HashMap<String, i64>,
}

impl Bucket {
    fn new(capacity: f64, refill_per_second: f64) -> Result<Self, &'static str> {
        if !capacity.is_finite() || capacity <= 0.0
            || !refill_per_second.is_finite() || refill_per_second <= 0.0
        {
            return Err("bad_config");
        }

        Ok(Self {
            capacity,
            refill_per_second,
            tokens: HashMap::new(),
            clock_ms: HashMap::new(),
        })
    }

    fn admit(
        &mut self,
        tenant: &str,
        now_ms: i64,
        cost: f64,
    ) -> Result<(bool, u64), &'static str> {
        if tenant.trim().is_empty()
            || now_ms < 0
            || !cost.is_finite()
            || cost <= 0.0
            || cost > self.capacity
        {
            return Err("bad_request");
        }

        let previous_ms = self.clock_ms.get(tenant).copied().unwrap_or(now_ms);
        if now_ms < previous_ms {
            return Err("clock_regressed");
        }

        let elapsed_ms = (now_ms - previous_ms) as f64;
        let current = self.tokens.get(tenant).copied().unwrap_or(self.capacity);
        let refill = (elapsed_ms / 1_000.0) * self.refill_per_second;
        let credit = (current + refill).min(self.capacity);

        // Invalid requests and clock regressions return before advancing state.
        self.clock_ms.insert(tenant.to_owned(), now_ms);
        if credit >= cost {
            self.tokens.insert(tenant.to_owned(), credit - cost);
            return Ok((true, 0));
        }

        self.tokens.insert(tenant.to_owned(), credit);
        let retry_after_ms = (((cost - credit) * 1_000.0) / self.refill_per_second)
            .ceil()
            .min(MAX_RETRY_AFTER_MS as f64) as u64;
        Ok((false, retry_after_ms.max(1)))
    }
}

#[test]
fn budget_and_backpressure_are_deterministic() {
    let mut bucket = Bucket::new(2.0, 1.0).expect("valid bucket");
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((true, 0)));
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((true, 0)));
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((false, 1_000)));
    assert_eq!(bucket.admit("tenant-a", 1_000, 1.0), Ok((true, 0)));
}

#[test]
fn tenant_budgets_are_isolated() {
    let mut bucket = Bucket::new(1.0, 1.0).expect("valid bucket");
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((true, 0)));
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((false, 1_000)));
    assert_eq!(bucket.admit("tenant-b", 0, 1.0), Ok((true, 0)));
}

#[test]
fn retry_after_is_bounded() {
    let mut bucket = Bucket::new(2.0, 0.001).expect("valid bucket");
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((true, 0)));
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((true, 0)));
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((false, MAX_RETRY_AFTER_MS)));
}

#[test]
fn clock_regression_fails_closed_without_rewinding_state() {
    let mut bucket = Bucket::new(2.0, 1.0).expect("valid bucket");
    assert_eq!(bucket.admit("tenant-a", 100, 1.0), Ok((true, 0)));
    assert_eq!(bucket.admit("tenant-a", 99, 1.0), Err("clock_regressed"));
    assert_eq!(bucket.admit("tenant-a", 100, 1.0), Ok((true, 0)));
    assert_eq!(bucket.admit("tenant-a", 100, 1.0), Ok((false, 1_000)));
}

#[test]
fn malformed_requests_and_configuration_fail_closed() {
    assert_eq!(Bucket::new(0.0, 1.0).err(), Some("bad_config"));
    assert_eq!(Bucket::new(1.0, 0.0).err(), Some("bad_config"));
    assert_eq!(Bucket::new(f64::INFINITY, 1.0).err(), Some("bad_config"));
    assert_eq!(Bucket::new(1.0, f64::NAN).err(), Some("bad_config"));

    let mut bucket = Bucket::new(1.0, 1.0).expect("valid bucket");
    assert_eq!(bucket.admit("", 0, 1.0), Err("bad_request"));
    assert_eq!(bucket.admit("   ", 0, 1.0), Err("bad_request"));
    assert_eq!(bucket.admit("tenant-a", -1, 1.0), Err("bad_request"));
    assert_eq!(bucket.admit("tenant-a", 0, 0.0), Err("bad_request"));
    assert_eq!(bucket.admit("tenant-a", 0, f64::NAN), Err("bad_request"));
    assert_eq!(bucket.admit("tenant-a", 0, f64::INFINITY), Err("bad_request"));
    assert_eq!(bucket.admit("tenant-a", 0, 2.0), Err("bad_request"));

    // Rejected requests do not consume tokens or advance the tenant clock.
    assert_eq!(bucket.admit("tenant-a", 0, 1.0), Ok((true, 0)));
}
