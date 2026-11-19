# Diagnostics Tab

The Diagnostics tab talks UDS (ISO 14229) to one ECU: it sends a request,
waits for the answer, and writes both in a log with a plain reading of what
each one means. Requests and answers longer than one CAN frame are split
and reassembled for you (ISO-TP, ISO 15765-2).

A capture has to be running on the ECU's bus. The requests and answers are
ordinary frames, so they also show up in the Trace view and in logs.

## ECU

Which ECU to talk to:

- **CAN n** - the bus it is on. Buses beyond CAN 1 are the ones ticked in
  [More buses](hardware-tab.md#more-buses).
- **Request ID** - the ID requests are sent on, in hex. Default `0x7E0`.
- **Response ID** - the ID the ECU answers on, in hex. Default `0x7E8`.
- **29-bit IDs** - tick for extended IDs such as `18DA10F1` / `18DAF110`.

These are saved in [runes](runes.md).

## Session

Pick **Default**, **Extended** or **Programming** and click **Switch**.
Most ECUs only allow writing and unlocking outside the default session.

**Tester present** sends a Tester Present message every two seconds. An
ECU drops back to its default session after a few seconds without hearing
from the tester, so tick this while you work in another session. These
messages are not written to the log, but you can see them in the Trace
view.

## Data

- **Read...** asks for a data identifier, four hex digits, and reads it.
  `F190` is the VIN. Standard identifiers are named in the log, and data
  that is all printable text is shown as text.
- **Write...** takes an identifier and the data to write, as hex bytes or
  as text in `"quotes"`.

## Trouble Codes

- **Read** lists every stored trouble code with its status, the way a scan
  tool prints them: `P0301-00 [testFailed, confirmedDTC]`.
- **Clear** erases them all, after asking.

## Control

- **Unlock...** runs Security Access level 1: it asks the ECU for a seed,
  shows it, and asks you for the key to send back. CANtrip does not know
  any manufacturer's key algorithm, so the key is yours to work out.
- **Reset ECU** sends a hard reset, after asking.

## Any other request

The **Request** line under the log sends any request you type as hex
bytes, for example `10 03` or `22 F1 90`. Press Enter or **Send**. The line
clears every time, and the log keeps what you typed.

## Reading the log

Each request and each answer is one row: the time, the bytes, and what
they mean.

- A refusal from the ECU (a negative response) is shown in red with the
  name of its reason, such as `securityAccessDenied`, and a sentence on
  what usually fixes it.
- "The ECU is working on it and will answer later" is the ECU asking for
  more time. CANtrip keeps waiting, up to five seconds.
- "No answer from the ECU" means nothing came back within a second. The
  usual cause is a wrong bus or wrong IDs.

Only one request is in flight at a time. The buttons are greyed out until
the answer, or the timeout, has arrived.

## Trying it without hardware

The synthetic test source has a small simulated ECU on **Request ID
`7E1`** and **Response ID `7E9`**. It is not on the default `7E0` / `7E8`
because the test source already sends a made-up periodic frame on `0x7E8`.

Start a capture on the synthetic source, set the two IDs, and try:

| Request | What happens |
| --- | --- |
| Read `F190` | The VIN, an answer three frames long |
| Read `0100` | An engine speed that changes over time |
| Trouble Codes, Read | Three trouble codes |
| Trouble Codes, Clear, then Read | None left. Reset ECU brings them back |
| Unlock in the default session | Refused: switch session first |
| Switch to Extended, then Unlock | A seed. The key is the seed XOR `CAFEBABE` |
| Wait five seconds, read `F186` | Back in the default session, unless Tester present is ticked |
| `31 01 FF 00` in the Request line | "Working on it" first, the result 0.4 s later |

## Limits

- Classic CAN frames only. Diagnostics over CAN FD frames is not built.
- Normal addressing only: one request ID and one response ID. No
  functional (broadcast) requests, no extended or mixed addressing.
- Answers reach CANtrip through the capture pipeline, which adds some tens
  of milliseconds. The timeouts are set generously for that reason, and an
  ECU with unusually strict flow control timing may not be satisfied.
- No ODX or CDD files: identifiers beyond the standard ones are shown as
  numbers.
- Not available in headless mode yet.
