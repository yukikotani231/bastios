use rayon::prelude::*;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Debug, PartialEq)]
struct Point {
    x: i32,
    y: i32,
}

fn main() {
    let sum: u64 = (1..=100_000u64).into_par_iter().map(|x| x * x % 7).sum();
    println!("{}", sum);
    let p = Point { x: 3, y: -4 };
    let s = serde_json::to_string(&p).unwrap();
    println!("{}", s);
    let q: Point = serde_json::from_str(&s).unwrap();
    println!("{}", q == p);
}
