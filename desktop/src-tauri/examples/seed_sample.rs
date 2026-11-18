//! Builds a self-contained demo world for looking at the desktop UI without
//! touching real data: a demo account, a demo device, tone recordings over two
//! days, and sample agent output (transcripts, summaries, day notes).
//!
//!   cargo run --example seed_sample -- <wav folder> <data dir> <library dir>
//!
//! `<wav folder>` holds WAVs named RECDDMMYYYY_HHMM.WAV (the name gives the
//! start time). `<data dir>` is the app's data folder for a *separate* app
//! identifier, so the real database is never opened, e.g.
//! `%APPDATA%\com.cprec.desktop.sample`. Then run the app with that identifier:
//!
//!   npm run tauri dev -- --config src-tauri/sample.tauri.json   (from desktop/)
//!
//! The account is "demo" / "demo1234" and the app opens straight into it.

use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
};

use chrono::{Local, NaiveDate, NaiveDateTime, TimeZone};
use cprec_desktop_lib::{
    agents::AgentCore,
    auth,
    db::{Db, RecordingRow},
    library,
};
use sha2::{Digest, Sha256};

const DEVICE_ID: &str = "00000000000000de";
const DEVICE_NAME: &str = "Demo Cardputer";

struct Sample {
    file: &'static str,
    transcript: Option<&'static str>,
    summary: Option<&'static str>,
    action_items: Option<&'static str>,
    translation_hi: Option<&'static str>,
}

const SAMPLES: &[Sample] = &[
    Sample {
        file: "REC17112026_0830.WAV",
        transcript: Some(
            "WEBVTT\n\n00:00.000 --> 00:06.500\nMorning. Flour delivery from Kapoor Mills is due at ten, forty bags.\n\n\
             00:06.500 --> 00:13.000\nCheck the invoice against the order before signing; last time two bags were short.\n\n\
             00:13.000 --> 00:19.800\nAlso call the oven technician, the left oven is still running cold.\n",
        ),
        summary: Some(
            "- Flour delivery from **Kapoor Mills** due at 10:00, 40 bags; check invoice against the order.\n\
             - Left oven still runs cold: technician needs a call.\n\
             - Last delivery was two bags short.\n",
        ),
        action_items: Some(
            "- [ ] Check the Kapoor Mills invoice against the order (40 bags) before signing\n\
             - [ ] Call the oven technician about the left oven\n",
        ),
        translation_hi: None,
    },
    Sample {
        file: "REC17112026_1615.WAV",
        transcript: Some(
            "WEBVTT\n\n00:00.000 --> 00:07.000\nCustomer on the phone about the wedding order for Saturday, 120 pieces, half with no nuts.\n\n\
             00:07.000 --> 00:14.500\nShe wants delivery by 5 pm. I said yes, but confirm with the kitchen first.\n\n\
             00:14.500 --> 00:21.000\nQuote was 18,000 rupees, she asked for a small discount for the early payment.\n",
        ),
        summary: Some(
            "- Wedding order for Saturday: **120 pieces**, half nut-free, delivery by 5 pm.\n\
             - Quoted 18,000 rupees; the customer asked for an early-payment discount.\n\
             - Kitchen must confirm capacity before it is final.\n",
        ),
        action_items: Some(
            "- [ ] Confirm the Saturday wedding order with the kitchen\n\
             - [ ] Decide on the early-payment discount (quote 18,000)\n",
        ),
        translation_hi: Some(
            "- शनिवार के लिए शादी का ऑर्डर: 120 पीस, आधे बिना मेवे के, शाम 5 बजे तक डिलीवरी।\n\
             - 18,000 रुपये का कोटेशन; ग्राहक ने जल्दी भुगतान पर छूट माँगी।\n\
             - अंतिम करने से पहले रसोई से पुष्टि ज़रूरी है।\n",
        ),
    },
    Sample {
        file: "REC18112026_0905.WAV",
        transcript: None,
        summary: Some(
            "- Supplier called: butter prices up about 6% from next month.\n\
             - Wants an answer on the standing order by Friday.\n",
        ),
        action_items: None,
        translation_hi: None,
    },
    Sample { file: "REC18112026_1130.WAV", transcript: None, summary: None, action_items: None, translation_hi: None },
];

const DAY_17_SUMMARY: &str = "# Monday 17 November\n\n\
    **Deliveries and a big order.** Flour from Kapoor Mills arrives in the morning (check the invoice: last time two \
    bags were short), and a 120-piece wedding order for Saturday is quoted at 18,000 rupees.\n\n\
    ## Decisions pending\n\
    - Early-payment discount for the wedding order\n\
    - Whether the kitchen can take Saturday's order\n\n\
    ## Open problems\n\
    - Left oven still runs cold\n";

const DAY_17_ACTIONS: &str = "- [ ] Check the Kapoor Mills invoice before signing (40 bags)\n\
    - [ ] Call the oven technician\n\
    - [ ] Confirm the wedding order with the kitchen\n\
    - [ ] Decide on the early-payment discount\n";

fn start_ms(file: &str) -> i64 {
    // RECDDMMYYYY_HHMM.WAV
    let stamp = &file[3..16];
    let naive = NaiveDateTime::new(
        NaiveDate::from_ymd_opt(stamp[4..8].parse().unwrap(), stamp[2..4].parse().unwrap(), stamp[0..2].parse().unwrap())
            .unwrap(),
        chrono::NaiveTime::from_hms_opt(stamp[9..11].parse().unwrap(), stamp[11..13].parse().unwrap(), 0).unwrap(),
    );
    Local.from_local_datetime(&naive).single().unwrap().timestamp_millis()
}

fn main() {
    let mut args = std::env::args().skip(1);
    let wavs = PathBuf::from(args.next().expect("usage: seed_sample <wav folder> <data dir> <library dir>"));
    let data = PathBuf::from(args.next().expect("data dir"));
    let lib = PathBuf::from(args.next().expect("library dir"));
    if data.join("cprec.sqlite").exists() {
        std::fs::remove_file(data.join("cprec.sqlite")).unwrap();
    }
    std::fs::create_dir_all(&data).unwrap();
    std::fs::create_dir_all(&lib).unwrap();

    let db = Db::open(&data.join("cprec.sqlite")).unwrap();
    let now = start_ms("REC18112026_1200.WAV");
    let hash = auth::hash_password("demo1234").unwrap();
    let user = db.create_user("demo", &hash, &lib.to_string_lossy(), now).unwrap();
    db.set_setting("remembered_user", Some(&user.id.to_string())).unwrap();
    db.set_setting("mcp_enabled", Some("1")).unwrap();
    db.set_setting("mcp_port", Some("47831")).unwrap();

    for s in SAMPLES {
        let bytes = std::fs::read(wavs.join(s.file)).expect("sample wav");
        let id = hex::encode(&Sha256::digest(&bytes)[..16]);
        let created = start_ms(s.file);
        let dest = library::final_path(&lib, DEVICE_NAME, s.file, &id, created, created);
        std::fs::create_dir_all(dest.parent().unwrap()).unwrap();
        std::fs::copy(wavs.join(s.file), &dest).unwrap();
        db.insert_recording(
            user.id,
            &RecordingRow {
                id,
                device_id: DEVICE_ID.into(),
                device_name: DEVICE_NAME.into(),
                name: s.file.into(),
                display_name: None,
                size: bytes.len() as u64,
                sha256: hex::encode(Sha256::digest(&bytes)),
                created_ms: created,
                duration_ms: (bytes.len().saturating_sub(44) as u64) * 1000 / 32_000,
                path: dest.to_string_lossy().to_string(),
                synced_at_ms: created + 60_000,
                acked: true,
                on_device: false,
            },
        )
        .unwrap();
    }

    let db = Arc::new(Mutex::new(db));
    let session = Arc::new(Mutex::new(Some(user.clone())));
    let core = AgentCore::new(db.clone(), session, Arc::new(|| {}));
    let (token, _) = core.create_token(user.id, "Sample agent", true, now).unwrap();
    let agent = core.authenticate(Some(&token), now).unwrap();

    let recordings = db.lock().unwrap().list_recordings(user.id).unwrap();
    for s in SAMPLES {
        let r = recordings.iter().find(|r| r.name == s.file).unwrap();
        let target = core.target_recording(user.id, &cprec_desktop_lib::agents::recording_ref(r)).unwrap();
        let put = |kind: &str, mime: &str, text: Option<&str>| {
            if let Some(text) = text {
                core.set_attribute(&agent, &target, kind, Some(mime), text, now).unwrap();
            }
        };
        put("transcript", "text/vtt", s.transcript);
        put("summary", "text/markdown", s.summary);
        put("action-items", "text/markdown", s.action_items);
        put("translation-hi", "text/markdown", s.translation_hi);
    }
    let day17 = AgentCore::target_day("2026-11-17").unwrap();
    core.set_attribute(&agent, &day17, "summary", Some("text/markdown"), DAY_17_SUMMARY, now).unwrap();
    core.set_attribute(&agent, &day17, "action-items", Some("text/markdown"), DAY_17_ACTIONS, now).unwrap();
    // The demo token is not needed afterwards: drop it so nothing is left to leak.
    let tokens = db.lock().unwrap().list_agent_tokens(user.id).unwrap();
    for t in tokens {
        db.lock().unwrap().delete_agent_token(user.id, t.id).unwrap();
    }
    println!("seeded {} recordings in {}; database in {}", SAMPLES.len(), lib.display(), data.display());
}
