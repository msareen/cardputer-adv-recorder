//! Local accounts: Argon2id password hashes stored in SQLite.

use argon2::{
    Argon2,
    password_hash::{PasswordHash, PasswordHasher, PasswordVerifier, SaltString, rand_core::OsRng},
};

pub fn hash_password(password: &str) -> Result<String, String> {
    let salt = SaltString::generate(&mut OsRng);
    Argon2::default() // Argon2id v19
        .hash_password(password.as_bytes(), &salt)
        .map(|h| h.to_string())
        .map_err(|e| e.to_string())
}

pub fn verify_password(password: &str, stored: &str) -> bool {
    PasswordHash::new(stored)
        .map(|parsed| Argon2::default().verify_password(password.as_bytes(), &parsed).is_ok())
        .unwrap_or(false)
}

pub fn validate_new_account(username: &str, password: &str) -> Result<(), String> {
    let name = username.trim();
    if name.is_empty() || name.len() > 64 {
        return Err("Username must be 1 to 64 characters.".into());
    }
    if !name.chars().all(|c| c.is_alphanumeric() || matches!(c, '-' | '_' | '.' | ' ')) {
        return Err("Username can use letters, digits, space, - _ and . only.".into());
    }
    if password.chars().count() < 8 {
        return Err("Password must be at least 8 characters.".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hashes_verify_and_are_salted() {
        let a = hash_password("correct horse").unwrap();
        let b = hash_password("correct horse").unwrap();
        assert!(a.starts_with("$argon2id$"));
        assert_ne!(a, b);
        assert!(verify_password("correct horse", &a));
        assert!(!verify_password("wrong", &a));
        assert!(!verify_password("x", "not a hash"));
    }

    #[test]
    fn validates_accounts() {
        assert!(validate_new_account("alice", "12345678").is_ok());
        assert!(validate_new_account("", "12345678").is_err());
        assert!(validate_new_account("a/b", "12345678").is_err());
        assert!(validate_new_account("alice", "short").is_err());
    }
}
