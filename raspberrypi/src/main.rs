use anyhow::{Context, Result};
use gpio_cdev::{Chip, LineHandle, LineRequestFlags};
use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::Write;
use std::net::{Ipv4Addr, SocketAddrV4, UdpSocket};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    mpsc,
    Arc, Mutex,
};
use std::thread;
use std::time::{Duration, Instant};

// GPIO (BCM pins)
const BUTTON_PIN: u32 = 26;
const WHITE_LED_PIN: u32 = 18;
const RGB_LED_PINS: [u32; 3] = [17, 22, 27];

// UDP / Protocol
const PORT: u16 = 4210;
const RPI_START: &str = "+++";
const RPI_END: &str = "***";

// Blink mapping
const X1: f64 = 24.0;
const Y1: f64 = 2010.0 / 1000.0;
const X2: f64 = 1024.0;
const Y2: f64 = 10.0 / 1000.0;

#[derive(Debug)]
struct SharedState {
    swarm_to_led: HashMap<String, usize>,
    next_led_index: usize,
    led_state: bool,
    previous_toggle: Instant,
}

impl SharedState {
    fn new() -> Self {
        Self {
            swarm_to_led: HashMap::new(),
            next_led_index: 0,
            led_state: false,
            previous_toggle: Instant::now(),
        }
    }

    fn assign_led_index(&mut self, swarm_id: &str) -> usize {
        if let Some(&idx) = self.swarm_to_led.get(swarm_id) {
            return idx;
        }
        let idx = self.next_led_index;
        self.swarm_to_led.insert(swarm_id.to_string(), idx);
        self.next_led_index = (self.next_led_index + 1) % 3;
        idx
    }

    fn reset(&mut self) {
        self.swarm_to_led.clear();
        self.next_led_index = 0;
        self.led_state = false;
        self.previous_toggle = Instant::now();
    }
}

enum GpioCmd {
    AllRgbOff,
    BlinkRgb { idx: usize, on: bool },
    WhiteOnFor3s,
}

fn open_chip() -> Result<Chip> {
    if let Ok(chip) = Chip::new("/dev/gpiochip4") {
        return Ok(chip);
    }
    Chip::new("/dev/gpiochip0").context("Failed to open /dev/gpiochip4 or /dev/gpiochip0")
}

fn request_input(chip: &mut Chip, pin: u32, name: &str) -> Result<LineHandle> {
    let line = chip
        .get_line(pin)
        .with_context(|| format!("get_line failed for pin {pin}"))?;
    line.request(LineRequestFlags::INPUT, 0, name)
        .with_context(|| format!("request INPUT failed for pin {pin}"))
}

fn request_output(chip: &mut Chip, pin: u32, name: &str, initial: u8) -> Result<LineHandle> {
    let line = chip
        .get_line(pin)
        .with_context(|| format!("get_line failed for pin {pin}"))?;
    line.request(LineRequestFlags::OUTPUT, initial, name)
        .with_context(|| format!("request OUTPUT failed for pin {pin}"))
}

fn set_led(line: &LineHandle, on: bool) {
    let _ = line.set_value(if on { 1 } else { 0 });
}

fn truncate_log() -> Result<()> {
    OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open("sensor_readings.txt")
        .context("Failed to truncate sensor_readings.txt")?;
    Ok(())
}

fn append_log(swarm_id: &str, reading: i32) -> Result<()> {
    let mut f = OpenOptions::new()
        .create(true)
        .append(true)
        .open("sensor_readings.txt")
        .context("Failed to open sensor_readings.txt for append")?;
    writeln!(f, "Swarm ID {}: {}", swarm_id, reading).context("Failed to write log line")?;
    Ok(())
}

fn parse_message(payload: &str) -> Option<(String, i32)> {
    if !payload.starts_with(RPI_START) || !payload.ends_with(RPI_END) {
        return None;
    }
    let inner = &payload[RPI_START.len()..payload.len() - RPI_END.len()];

    if inner == "RESET_REQUESTED" {
        return None;
    }

    let parts: Vec<&str> = inner.split(',').map(|s| s.trim()).collect();
    match parts.as_slice() {
        [swarm_id, reading] => {
            let reading: i32 = reading.parse().ok()?;
            Some((swarm_id.to_string(), reading))
        }
        [role, swarm_id, reading] => {
            if *role != "Master" {
                return None;
            }
            let reading: i32 = reading.parse().ok()?;
            Some((swarm_id.to_string(), reading))
        }
        _ => None,
    }
}

fn blink_interval_seconds(reading: i32) -> f64 {
    let slope = (Y2 - Y1) / (X2 - X1);
    let intercept = Y1 - slope * X1;

    let x = (reading as f64).clamp(0.0, X2);
    let mut seconds = slope * x + intercept;
    if seconds < 0.005 {
        seconds = 0.005;
    }
    seconds
}

fn main() -> Result<()> {
    // UDP init
    let sock = UdpSocket::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, PORT))
        .with_context(|| format!("Failed to bind UDP port {PORT}"))?;
    sock.set_broadcast(true).context("Failed to enable broadcast")?;
    sock.set_read_timeout(Some(Duration::from_millis(100)))
        .context("Failed to set read timeout")?;

    let sock_send = sock.try_clone().context("Failed to clone UDP socket")?;

    println!("Listening on UDP port {PORT}...");

    // Shared state
    let reset_flag = Arc::new(AtomicBool::new(false));
    let state = Arc::new(Mutex::new(SharedState::new()));

    // GPIO command channel
    let (tx, rx) = mpsc::channel::<GpioCmd>();

    // GPIO thread owns ALL gpio handles (NO CLONE)
    let reset_flag_gpio = Arc::clone(&reset_flag);
    let state_gpio = Arc::clone(&state);

    let gpio_thread = thread::spawn(move || -> Result<()> {
        let mut chip = open_chip()?;
        let button = request_input(&mut chip, BUTTON_PIN, "button")?;
        let white_led = request_output(&mut chip, WHITE_LED_PIN, "white_led", 0)?;

        let mut rgb_leds: Vec<LineHandle> = Vec::new();
        for (i, pin) in RGB_LED_PINS.iter().enumerate() {
            let h = request_output(&mut chip, *pin, &format!("rgb_led_{i}"), 0)?;
            rgb_leds.push(h);
        }

        set_led(&white_led, false);
        for led in &rgb_leds {
            set_led(led, false);
        }

        let mut prev_btn = 0;

        loop {
            // process gpio commands
            while let Ok(cmd) = rx.try_recv() {
                match cmd {
                    GpioCmd::AllRgbOff => {
                        for led in &rgb_leds {
                            set_led(led, false);
                        }
                    }
                    GpioCmd::BlinkRgb { idx, on } => {
                        if idx < rgb_leds.len() {
                            for (i, led) in rgb_leds.iter().enumerate() {
                                if i != idx {
                                    set_led(led, false);
                                }
                            }
                            set_led(&rgb_leds[idx], on);
                        }
                    }
                    GpioCmd::WhiteOnFor3s => {
                        set_led(&white_led, true);
                        thread::sleep(Duration::from_secs(3));
                        set_led(&white_led, false);
                    }
                }
            }

            // button press
            let v = button.get_value().unwrap_or(0);
            if v == 0 && prev_btn == 1 {
                reset_flag_gpio.store(true, Ordering::SeqCst);

                // broadcast reset
                let msg = format!("{RPI_START}RESET_REQUESTED{RPI_END}");
                let bcast = SocketAddrV4::new(Ipv4Addr::new(255, 255, 255, 255), PORT);
                let _ = sock_send.send_to(msg.as_bytes(), bcast);

                // clear log + reset state
                let _ = truncate_log();
                {
                    let mut st = state_gpio.lock().unwrap();
                    st.reset();
                }

                // LEDs
                for led in &rgb_leds {
                    set_led(led, false);
                }
                set_led(&white_led, true);
                thread::sleep(Duration::from_secs(3));
                set_led(&white_led, false);

                reset_flag_gpio.store(false, Ordering::SeqCst);
            }
            prev_btn = v;

            thread::sleep(Duration::from_millis(50));
        }
    });

    // UDP receive loop in main thread
    let mut buf = [0u8; 1024];
    loop {
        if reset_flag.load(Ordering::SeqCst) {
            thread::sleep(Duration::from_millis(50));
            continue;
        }

        match sock.recv_from(&mut buf) {
            Ok((n, _addr)) => {
                let payload = match std::str::from_utf8(&buf[..n]) {
                    Ok(s) => s,
                    Err(_) => continue,
                };

                let Some((swarm_id, reading)) = parse_message(payload) else {
                    continue;
                };

                println!("Swarm {} -> {}", swarm_id, reading);
                let _ = append_log(&swarm_id, reading);

                let led_index = {
                    let mut st = state.lock().unwrap();
                    st.assign_led_index(&swarm_id)
                };

                let interval = Duration::from_secs_f64(blink_interval_seconds(reading));

                let on = {
                    let mut st = state.lock().unwrap();
                    if st.previous_toggle.elapsed() >= interval {
                        st.previous_toggle = Instant::now();
                        st.led_state = !st.led_state;
                    }
                    st.led_state
                };

                let _ = tx.send(GpioCmd::BlinkRgb { idx: led_index, on });
            }
            Err(e) => {
                if e.kind() != std::io::ErrorKind::WouldBlock
                    && e.kind() != std::io::ErrorKind::TimedOut
                {
                    eprintln!("UDP recv error: {e}");
                }
            }
        }
    }

    // unreachable
    // let _ = gpio_thread.join();
    // Ok(())
}
