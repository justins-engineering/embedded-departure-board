#!/usr/bin/env python3
"""Redact a baked device credential from a Zephyr dictionary-log database.

database_gen.py collects the image's whole static rodata string pool so that %s
pointers resolve, so any build that bakes CONFIG_PIGEON_TOKEN carries that live
token in log_dictionary.json. Decoding is keyed by address, so replacing the
value leaves every real log line decodable.

Usage: sanitize_log_dictionary.py <log_dictionary.json> <prj.local.conf> [-o OUT]
       sanitize_log_dictionary.py <log_dictionary.json> --legacy --expect N [-o OUT]
"""

import argparse
import json
import re
import sys
from pathlib import Path

REDACTED = "<redacted device credential>"
TOKEN_LINE = re.compile(r'^\s*CONFIG_PIGEON_TOKEN\s*=\s*"(.+)"\s*$')
# A platform device token's shape. Matched whole-value in --legacy mode, so an
# endpoint URL carrying a 64-hex pigeon id cannot be mistaken for one.
SHAPE = r"[A-Za-z0-9_-]{88,96}"
CREDENTIAL_VALUE = re.compile(SHAPE)
# One of these surviving anywhere in the output means a copy was missed.
CREDENTIAL_RUN = re.compile(r"(?<![A-Za-z0-9_-])" + SHAPE + r"(?![A-Za-z0-9_-])")


def fail(message):
  sys.exit("sanitize_log_dictionary: " + message)


def read_token(conf):
  """Return CONFIG_PIGEON_TOKEN's value. Never log or echo it."""
  for line in conf.read_text(encoding="utf-8").splitlines():
    match = TOKEN_LINE.match(line)
    if match:
      return match.group(1)
  fail("no CONFIG_PIGEON_TOKEN in " + str(conf))


def redact(mappings, token):
  """Redact by known value, or by shape when the build's credential is gone."""
  count = 0
  for address, value in mappings.items():
    hit = token in value if token else CREDENTIAL_VALUE.fullmatch(value)
    if hit:
      mappings[address] = REDACTED
      count += 1
  return count


def main():
  parser = argparse.ArgumentParser(description="Redact a device token from a log dictionary.")
  parser.add_argument("dictionary", type=Path, help="log_dictionary.json to sanitize")
  parser.add_argument("conf", type=Path, nargs="?", help="prj.local.conf holding the token")
  parser.add_argument("-o", "--output", type=Path, help="default: <dictionary>.sanitized.json")
  parser.add_argument("--legacy", action="store_true",
                      help="redact by shape, for a build whose credential is no longer on hand")
  parser.add_argument("--expect", type=int, help="exact number of values --legacy must redact")
  args = parser.parse_args()

  if args.legacy:
    if args.conf is not None or args.expect is None or args.expect < 1:
      fail("--legacy takes --expect N (N >= 1) and no conf")
  elif args.conf is None:
    fail("a conf is required unless --legacy is given")

  token = None if args.legacy else read_token(args.conf)
  database = json.loads(args.dictionary.read_text(encoding="utf-8"))
  mappings = database.get("string_mappings")
  if not isinstance(mappings, dict):
    fail("no string_mappings object in " + str(args.dictionary))

  redacted = redact(mappings, token)
  if args.legacy and redacted != args.expect:
    fail("redacted " + str(redacted) + " values, expected exactly " + str(args.expect))
  if not redacted:
    fail("token absent from " + str(args.dictionary) + "; wrong conf for this build?")

  # Same serialization as database_gen.py, so the file differs only by the value.
  # Verified before it is written, so a failed check never leaves a file to upload.
  payload = json.dumps(database)
  if token and token in payload:
    fail("token survives the redaction")
  survivors = len(CREDENTIAL_RUN.findall(payload))
  if survivors:
    fail(str(survivors) + " credential-shaped strings survive the redaction")

  out = args.output or args.dictionary.with_suffix(".sanitized.json")
  out.write_text(payload, encoding="utf-8")
  try:
    check = json.loads(out.read_text(encoding="utf-8"))
  except json.JSONDecodeError as error:
    out.unlink()
    fail("output is not valid JSON: " + str(error))
  if len(check.get("string_mappings", {})) != len(mappings):
    out.unlink()
    fail("output entry count changed")

  print("redacted " + str(redacted) + " of " + str(len(mappings)) + " entries -> " + str(out))


if __name__ == "__main__":
  main()
