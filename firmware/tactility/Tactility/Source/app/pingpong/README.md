# Ping Pong App

Two-player Pong over ESP-NOW. No access point or internet is needed: badges find each other by
broadcasting presence, one player challenges another, the other confirms, and the match runs
directly between them over an encrypted link.

## Requirements

- Two ESP32 devices with WiFi support (not available on ESP32-P4)
- Nothing else: the app disconnects from any access point while it runs, so both badges settle on
  channel 1 and can always see each other. WiFi is restored when the app exits.

## Playing

1. Open Ping Pong on both devices. Each shows its own name and the nearby badges, strongest
   signal first, with the RSSI of each.
2. Select an opponent and press OK to challenge them.
3. The other badge shows the challenge with a four-digit match code, and its player chooses Play
   or Decline. The code is on that screen, where the decision is made; play starts immediately
   on Play.
4. The challenger hosts and plays the left paddle; the challenged player plays the right paddle.
   Up/Down move the paddle. First to 5 points wins.
   A serve waits at the centre before it sets off, and a serve that is missed before either
   paddle has touched it costs nothing: it is simply served again to the same player.
5. The final score stays on the field, with Play again selected by default and Exit beside it.
   Play again restarts against the same opponent on the same key, without a new handshake.
6. Back leaves the match; from the lobby, the toolbar back button exits the app.

## Identity and names

A badge's identity is its WiFi station MAC, and its display name is the last three bytes of it
(`Badge-AB12CD`). Names are therefore unique between badges from the same production batch,
rather than merely unlikely to collide, which matters in a room with hundreds of them.

On an ESP32-P4 with a hosted WiFi co-processor the advertised identity is the host chip's MAC
while frames carry the co-processor's address. Both are tracked separately, so this works, but
the displayed name is not derived from the address other badges see on the air.

## Discovery at scale

Presence is broadcast every 3 seconds plus up to 1 second of jitter, so a crowded room does not
line its announcements up. A badge is forgotten after 12 seconds without a Hello.

Airtime, not memory, is the limit here: a short ESP-NOW broadcast costs roughly a millisecond of
channel time at the basic rate, so 400 badges announcing once every 3.5 seconds average is around
11% channel occupancy, and each running match adds roughly 4% on top.

At most 64 badges are held in memory, the weakest signal being dropped first, and the 20 nearest
are listed. The list refreshes once per second and keeps the focus on the same badge across
refreshes, so it does not jump under the player's fingers.

## Security

Each match runs on a fresh key. The challenger and the challenged badge exchange X25519 public
keys in the Invite and Accept packets, and both derive:

- a 16-byte ESP-NOW local master key, `SHA-256("tactility-pingpong-lmk" || secret || pubA || pubB)`
- a four-digit match code from the same transcript under a different label

Match traffic then runs as *encrypted unicast* to that peer, so CCMP encryption and integrity run
in the WiFi MAC hardware — no per-packet cost on the CPU — and unicast adds link-layer
acknowledgement and retry, which broadcast does not have. The two public keys are sorted before
hashing, so both sides derive the same values regardless of who spoke first.

The key exchange is unauthenticated, which stops every off-path attacker but not an active one
sitting between the two badges during the handshake. The four-digit code narrows that gap: an attacker who
relays the handshake cannot produce the code the challenged badge expects.

The challenged badge sees the code on the screen where it accepts, and both badges show it in the
corner of the field for the whole match, so the challenger - which cannot compute it until the
answer arrives - can still check that the two agree.

The ESP32-S3 has AES, SHA and HMAC accelerators but no ECC accelerator, so the two X25519 scalar
multiplications run in software. They happen once per match, before play starts, and nothing in
the match loop performs cryptography on the CPU.

Remaining limitations: the lobby is plaintext, so anyone in range can see who is present and who
is challenging whom, and a badge can announce an identity that is not its own.

## Networking

Lobby packets (Hello, Invite, Accept, Decline) are broadcast and addressed by identity in the
header. Match packets (State, Input, Bye) are unicast to the encrypted peer.

The host owns the simulation: it advances the ball, resolves paddle collisions and keeps score,
sending its state at 20Hz. The guest sends only its paddle position at the same rate. Both players
draw their own paddle from local input, so it tracks the keys without waiting for a round trip.

State carries the ball's velocity as well as its position, and the guest carries the ball on with
that velocity between updates. Without it the guest's ball only moves when a packet lands, so it
sits still between updates and stops dead on any that go missing; bounces, scoring and serving
stay with the host, whose next state corrects any drift.

A rematch reuses the key and the encrypted peer already established. The host restarting is the
whole signal: its next state carries `over == 0`, which is what moves the guest into the new
match. A guest that asks first sends Rematch, repeating it until the host restarts.

If nothing is heard from the opponent for 4 seconds, the match is abandoned and the app returns
to the lobby.

### Scoring

A point is only awarded for a ball that was in play - one that a paddle has returned at least
once since the serve. Missing the ball that was served to you is not a miss anyone gets credit
for, so the ball is served again to the same player. Combined with the pause at the centre before
each serve, that stops a player who is out of position from shedding several points in a row.

### Collision

The ball travels further in a tick than the paddle is thick, so the host tests where the ball's
leading edge crossed the paddle's plane during the step rather than where it ended up, and takes
the contact height from that crossing. Testing the landing position lets a fast ball step over a
paddle it should have hit.

The guest's paddle position is sent every tick rather than every other one: the host's hit test is
only as good as its idea of where that paddle is, and a half-step-stale position reads to the
player as the ball passing straight through the bat.

### Packet format

Header (19 bytes), little-endian:

```text
Offset  Size   Field
------  ----   -----
0       4      magic ('P','I','N','G')
4       1      version (1)
5       1      type
6       1      session (match id, chosen by the challenger)
7       6      from (identity address)
13      6      to (identity address, FF:FF:FF:FF:FF:FF = broadcast)
```

| Type | Value | Payload |
|------|-------|---------|
| Hello | 1 | none |
| Invite | 2 | X25519 public key (32 bytes) |
| Accept | 3 | X25519 public key (32 bytes) |
| Decline | 4 | none |
| State | 5 | ballX, ballY, ballVx, ballVy, hostPaddleY, guestPaddleY (int16), hostScore, guestScore, over (uint8) |
| Input | 6 | paddleY (int16) |
| Bye | 7 | none |
| Rematch | 8 | none |

Positions are in a resolution-independent 1000x1000 unit field that each device maps onto its own
display, so devices with different screen sizes agree on the physics.

Packets that fail validation (magic, version, unknown type, wrong payload size) are dropped, as
are packets from another session or from a badge that is not the current opponent.

## Presentation

The match is played full screen: the app draws no toolbar of its own, and the back key still
reaches the window manager because the badge's keypad driver calls `lvgl_toolbar_trigger_back()`
directly. The system status bar is left alone. Buttons and lists are drawn from a small style set
rather than the default theme, so focus is shown by filling a button in the accent colour, which
is far easier to follow on the badge than the theme's outline.

## Latency

ESP-NOW is carried by the WiFi radio, so the radio cannot be switched off for a match. Modem
sleep is switched off instead, for the duration of the match only: with power saving on, a badge
associated with an access point parks its receiver between beacons, which delays paddle updates
on their way to the host.

The app also leaves any access point for as long as it runs, and rejoins on the way out. This is
not only about latency: an associated badge follows its access point's channel rather than the
one ESP-NOW asks for, so two badges on different networks never see each other, and one sharing
the air with a busy network watches its paddle updates queue behind that traffic. Disconnected,
both badges land on the configured channel.

The service's auto-connect scan is paused while the app runs, otherwise it would put the badge
straight back onto the network it was just taken off. On exit the scan is resumed and, if the
badge was connected when the app started, it is reconnected to that same network.

## Input handling

The badge keypad only reports arrow keys when the LVGL group is in edit mode, so the match screen
claims focus for the field object and forces edit mode. Keypad auto-repeat is also shortened
while playing (90ms to first repeat, 30ms between) and restored when the match screen goes away.

## Handshake recovery

Invite, Accept, Decline and Rematch are broadcast and unacknowledged, so each is repeated until
it is answered. The challenger repeats its Invite while waiting; a badge that declined answers a
repeated Invite with the decline again rather than asking its player twice; and the accepting
badge repeats its Accept until the host's first state proves it was heard. Without this, a single
lost frame costs both badges their timeout, one reporting silence and the other a refusal.

Elapsed time is always measured with `lv_tick_elaps()` rather than against a tick sampled earlier
in the same pass. A phase entered part-way through a pass starts later than that sample, and the
unsigned subtraction wraps to roughly 4.3 billion, firing every timeout at once.

## Limitations

- One match at a time, and no spectating.
- If two badges challenge each other simultaneously, both invites are ignored and both time out;
  challenge again from one side only.
- Removing an ESP-NOW peer is unsupported on the hosted (ESP32-P4) backend, so on that platform
  the encrypted peer stays until ESP-NOW is torn down.
