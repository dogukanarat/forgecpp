# Security policy

## Current state

forgecpp is pre-1.0 and is used in labs and on trusted networks. Its
security features are not complete, and some defaults are open:

- Without an `[auth]` section, anyone who can reach the HTTP port, the control
  endpoint (`[forge].control`) or the telnet console can read and change
  everything. A program logs a warning when it starts like this.
- HTTP has no TLS of its own; telnet is plain text.
- Commands that arrive on a link (ZeroMQ, serial, CAN) carry no credentials and
  get `[auth].default_role`.

Run forgecpp programs on trusted networks only, or put them behind a firewall,
a VPN or a TLS-terminating proxy. The planned security work is in
[docs/ROADMAP.md](docs/ROADMAP.md#3-security).

## Supported versions

| Version | Security fixes |
|---|---|
| 0.4.x (the `master` branch) | yes |
| older | no |

## Reporting a vulnerability

Please do not open a public issue for a security problem. Report it
privately through GitHub: the repository's **Security** tab, **Report a
vulnerability**. Include what is affected, how to reproduce it, and the
impact you see.

You will get an answer within a week. Once the problem is understood, a fix
is prepared in private, released, and the advisory published with credit to
you (unless you prefer otherwise).

Known gaps listed above (no authentication by default, no TLS, telnet) are
not vulnerabilities in themselves, but reports of ways they can be abused
beyond what is described, or of anything else, are welcome.
