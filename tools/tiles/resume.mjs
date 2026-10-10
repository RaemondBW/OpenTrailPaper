#!/usr/bin/env node
// Finish an earlier run instead of starting over: reuse its plan and redo
// only the jobs that failed. Interior jobs that succeeded are not rebuilt —
// the border jobs read their strips from the earlier run's artifacts (kept
// two days) — and the merge runs as a partial one, so every fragment the
// earlier run already published stays.
//
//   node tools/tiles/resume.mjs --plan plan.json --jobs jobs.tsv --out plan.json
// jobs.tsv: "<job name>\t<conclusion>" per line, from the Actions API, e.g.
// "interior (j03)\tfailure". Prints {"interior":[…],"border":[…]}.
import fs from "node:fs";
import { NOT_IN_PLANET } from "./plan.mjs";

export function resume(plan, results) {
  const failed = (phase) => plan.jobs.map((j) => j.name).filter((n) => results.get(`${phase} (${n})`) !== "success");
  const interior = failed("interior");
  // A border job reruns when it failed or when an interior job it reads is
  // rebuilt (its strips change, and they come from this run then).
  const border = plan.jobs.filter((j) => results.get(`border (${j.name})`) !== "success" || j.needs.some((n) => interior.includes(n)))
    .map((j) => j.name);
  // Regions since left out of a planet run (Antarctica broke H3).
  const drop = (id) => NOT_IN_PLANET.has(id);
  const out = {
    ...plan,
    regions: plan.regions.filter((r) => !drop(r.id)),
    jobs: plan.jobs.map((j) => ({ ...j, regions: j.regions.filter((id) => !drop(id)) })),
    partial: true,
  };
  return { plan: out, interior, border };
}

async function main() {
  const a = {};
  for (let i = 2; i < process.argv.length; i += 2) a[process.argv[i].replace(/^--/, "")] = process.argv[i + 1];
  if (!a.plan || !a.jobs || !a.out) { console.error("usage: resume.mjs --plan plan.json --jobs jobs.tsv --out plan.json"); process.exit(2); }
  const plan = JSON.parse(fs.readFileSync(a.plan, "utf8"));
  const results = new Map(fs.readFileSync(a.jobs, "utf8").split("\n").filter(Boolean).map((l) => l.split("\t")));
  const r = resume(plan, results);
  fs.writeFileSync(a.out, JSON.stringify(r.plan));
  console.error(`resume: interior ${r.interior.join(" ") || "none"}; border ${r.border.join(" ") || "none"}`);
  console.log(JSON.stringify({ interior: r.interior, border: r.border }));
}

if (process.argv[1] && import.meta.url === `file://${(await import("node:path")).resolve(process.argv[1])}`) await main();
