// test_relay_find.cpp — the auto relay discovery (task #160): the DECISIONS
// of relay_find_logic.h (the hash and MAC an answer must carry, the order it
// looks in — stored address, broadcast, sweep of the subnet — what it refuses
// to believe, when it looks, what it stores) and the JOINS of relay_find.h
// (the packets that go out, the socket that is opened and closed, what ends
// up in the flash relay configuration), compiled against the stub Arduino
// environment like test_net_logic.cpp. The relay's half of the protocol is
// tested in nodejs/esp-relay/tests/test_discovery.js against the same fixed
// vector, and over real sockets by tests/e2e/test_discovery_e2e.js, so the
// C++ and the JavaScript cannot drift apart unnoticed.

#include <cassert>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../stubs/WiFiUdp.h" // pulls in Arduino.h + the fake datagram world

std::string Stream::serialOut;
static EspStub espInstance;
static WiFiStub wifiInstance;
EspStub& ESP = espInstance;
WiFiStub& WiFi = wifiInstance;
Stream Serial;
static unsigned long fakeNow = 1000;
unsigned long millis() { return fakeNow; }
void delay(unsigned long ms) { fakeNow += ms; }   // the fake clock moves while the code waits, so the glue's receive loops end

#include "../../arduino/ESP_node/relay_find_logic.h"
#include "../../arduino/ESP_node/relay_find.h"

using namespace espnet_relayfind;

static const char* NONCE = "0123456789abcdef";
static const char* KEY = "change-me";

static std::string digestHex(const std::string& s) {
  Sha256 h;
  h.update(s);
  uint8_t d[32];
  h.finish(d);
  return toHex(d, 32);
}

// ── SHA-256 / HMAC against published vectors (also checked with sha256sum / openssl) ──

static void testSha256KnownAnswers() {
  assert(digestHex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(digestHex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(digestHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"); // two blocks (FIPS 180-4)
  assert(digestHex(std::string(1000000, 'a')) ==
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); // a million a's
  printf("PASS sha256 known answers\n");
}

static void testSha256PaddingEdges() {
  // 55, 56, 63, 64, 65 bytes straddle the padding rules; digests worked out with
  //   head -c N /dev/zero | tr '\0' 'x' | sha256sum
  assert(digestHex(std::string(55, 'x')) == "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072");
  assert(digestHex(std::string(56, 'x')) == "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e");
  assert(digestHex(std::string(63, 'x')) == "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2");
  assert(digestHex(std::string(64, 'x')) == "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c");
  assert(digestHex(std::string(65, 'x')) == "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9");
  // ...and the same input fed in odd-sized pieces gives the same digest (the buffering across blocks).
  const size_t sizes[] = {55, 56, 63, 64, 65, 119, 120, 127, 128, 129};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    std::string s(sizes[i], 'q');
    Sha256 pieces;
    for (size_t at = 0; at < s.size();) {
      size_t n = (at % 7) + 1;
      if (at + n > s.size()) n = s.size() - at;
      pieces.update(reinterpret_cast<const uint8_t*>(s.data()) + at, n);
      at += n;
    }
    uint8_t d[32];
    pieces.finish(d);
    assert(toHex(d, 32) == digestHex(s));
  }
  printf("PASS sha256 padding edges\n");
}

static void testHmacRfc4231Vectors() {
  assert(hmacSha256Hex(std::string(20, '\x0b'), "Hi There") ==
         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"); // RFC 4231 case 1
  assert(hmacSha256Hex("Jefe", "what do ya want for nothing?") ==
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"); // RFC 4231 case 2
  assert(hmacSha256Hex(std::string(20, '\xaa'), std::string(50, '\xdd')) ==
         "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"); // RFC 4231 case 3
  assert(hmacSha256Hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") ==
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"); // RFC 4231 case 6
  printf("PASS hmac rfc4231 vectors\n");
}

static void testFleetMacVector() {
  // The same fixed vector nodejs/esp-relay/tests/test_discovery.js checks the relay's side against;
  // it was worked out with: printf 'ESPNET1!0123456789abcdef' | openssl dgst -sha256 -hmac 'change-me'
  const std::string vector = "0259fef1f2b658a2504d643ffcc048ee8bddccd5578ae388a7baafaa9463f105";
  assert(hmacSha256Hex(KEY, std::string("ESPNET1!") + NONCE) == vector);
  assert(replyValid(std::string("ESPNET1!") + NONCE + ":" + vector, NONCE, KEY));
  printf("PASS the fleet's MAC vector (matches the relay's test)\n");
}

// ── What a node believes ──

static void testReplyValidation() {
  const std::string mac = hmacSha256Hex(KEY, std::string("ESPNET1!") + NONCE);
  const std::string good = std::string("ESPNET1!") + NONCE + ":" + mac;
  assert(replyValid(good, NONCE, KEY));                                             // the real answer
  assert(!replyValid(good, NONCE, "other-key"));                                    // checked with another key
  assert(!replyValid(good, "fedcba9876543210", KEY));                               // a replayed answer
  assert(!replyValid(good + "0", NONCE, KEY));                                      // longer
  assert(!replyValid(good.substr(0, good.size() - 1), NONCE, KEY));                 // shorter
  std::string flipped = good;
  flipped[flipped.size() - 1] = flipped[flipped.size() - 1] == '0' ? '1' : '0';
  assert(!replyValid(flipped, NONCE, KEY));                                         // one changed character
  std::string upper = good;
  for (size_t i = 0; i < upper.size(); ++i) if (upper[i] >= 'a' && upper[i] <= 'f') upper[i] = (char)(upper[i] - 32);
  assert(!replyValid(upper, NONCE, KEY));                                           // upper-case
  assert(!replyValid(std::string("ESPNET1?") + NONCE + ":" + mac, NONCE, KEY));     // the probe's tag is not the answer's
  assert(!replyValid(std::string("ESPNET1!") + NONCE + ":" + hmacSha256Hex("wrong", std::string("ESPNET1!") + NONCE), NONCE, KEY));
  assert(!replyValid("yes I am the relay", NONCE, KEY));                            // an answer without proof
  assert(!replyValid("", NONCE, KEY));
  assert(!replyValid(good, NONCE, ""));                        // no key: nothing is believed
  assert(!replyValid(good, "short", KEY));                     // a malformed nonce
  printf("PASS reply validation\n");
}

static void testProbeText() {
  assert(buildProbe(NONCE) == "ESPNET1?0123456789abcdef");
  assert(isNonce("0123456789abcdef") && isNonce("ffffffffffffffff"));
  assert(!isNonce("") && !isNonce("0123456789abcde") && !isNonce("0123456789abcdef0") &&
         !isNonce("0123456789ABCDEF") && !isNonce("0123456789abcdeg"));
  printf("PASS probe text\n");
}

static void testIpv4Parsing() {
  uint8_t a[4];
  assert(parseIpv4("192.168.1.9", a) && a[0] == 192 && a[1] == 168 && a[2] == 1 && a[3] == 9);
  assert(parseIpv4("0.0.0.0", a) && parseIpv4("255.255.255.255", a));
  const char* bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.256", "01.2.3.4", "1.2.3.04", "1..3.4", "1.2.3.", ".1.2.3",
                       "a.b.c.d", "1.2.3.4 ", " 1.2.3.4", "raspberrypi.local", "1.2.3.4a", "-1.2.3.4", "1234.1.1.1", "1.2.3.0x4"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) assert(!parseIpv4(bad[i], a));
  uint8_t base[4] = {10, 0, 7, 99};
  assert(ipv4With(base, 5) == "10.0.7.5");
  printf("PASS ipv4 parsing\n");
}

// ── A scripted network (the locator's decisions) ──

struct FakeNet : IFindEnv {
  uint32_t now = 1000;
  std::string own = "192.168.1.50";
  std::map<std::string, std::string> dns;            // name -> address
  std::string nonce = NONCE;
  std::map<std::string, std::string> relays;         // address -> key it answers with (anyone with the wrong key is a rogue)
  bool broadcastReaches = true;                      // false: a NAT router in between
  bool unicastReaches = true;
  int dropFirstUnicasts = 0;                         // lose this many unicast probes (UDP is not reliable)
  std::vector<std::string> sentTo;
  std::deque<std::pair<std::string, std::string> > inbox;   // answers waiting to be read

  uint32_t nowMs() override { return now; }
  void pauseMs(uint32_t ms) override { now += ms; }
  std::string newNonce() override { return nonce; }
  bool resolve(const std::string& host, std::string& ip) override {
    uint8_t a[4];
    if (parseIpv4(host, a)) { ip = host; return true; }
    std::map<std::string, std::string>::const_iterator it = dns.find(host);
    if (it == dns.end()) return false;
    ip = it->second;
    return true;
  }
  std::string ownIp() override { return own; }
  std::string answer(const std::string& key, const std::string& n) {
    std::string signedText = std::string("ESPNET1!") + n;
    return signedText + ":" + hmacSha256Hex(key, signedText);
  }
  bool send(const std::string& ip, const std::string& payload) override {
    sentTo.push_back(ip);
    if (payload.size() != 24 || payload.compare(0, 8, "ESPNET1?") != 0) return false;
    std::string n = payload.substr(8);
    if (ip == "255.255.255.255") {
      if (!broadcastReaches) return true;
      for (std::map<std::string, std::string>::const_iterator r = relays.begin(); r != relays.end(); ++r)
        inbox.push_back(std::make_pair(r->first, answer(r->second, n)));
      return true;
    }
    if (!unicastReaches) return true;
    if (dropFirstUnicasts > 0) { --dropFirstUnicasts; return true; }
    std::map<std::string, std::string>::const_iterator r = relays.find(ip);
    if (r != relays.end()) inbox.push_back(std::make_pair(ip, answer(r->second, n)));
    return true;
  }
  bool receive(uint32_t waitMs, std::string& fromIp, std::string& payload) override {
    if (inbox.empty()) { now += waitMs; return false; }
    fromIp = inbox.front().first;
    payload = inbox.front().second;
    inbox.pop_front();
    return true;
  }
  int countTo(const std::string& ip) const {
    int n = 0;
    for (size_t i = 0; i < sentTo.size(); ++i) if (sentTo[i] == ip) ++n;
    return n;
  }
};

static void testFindsItWhereItWasWithOneProbe() {
  FakeNet net;
  net.relays["192.168.1.9"] = KEY;
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(l.ok && l.ip == "192.168.1.9" && l.atConfigured);
  assert(net.sentTo.size() == 1);   // one packet, no broadcast, no sweep
  printf("PASS found at the stored address with one probe\n");
}

static void testALostFirstProbeIsTriedAgain() {
  FakeNet net;
  net.relays["192.168.1.9"] = KEY;
  net.dropFirstUnicasts = 1;
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(l.ok && l.atConfigured);
  assert(net.sentTo.size() == 2);
  printf("PASS a lost first probe is tried again\n");
}

static void testANameThatResolvesIsProbedFirst() {
  FakeNet net;
  net.dns["raspberrypi.local"] = "192.168.1.9";
  net.relays["192.168.1.9"] = KEY;
  Located l = RelayLocator(net).locate("raspberrypi.local", KEY);
  assert(l.ok && l.ip == "192.168.1.9" && l.atConfigured);
  assert(net.sentTo.size() == 1);
  printf("PASS a name that resolves is probed first\n");
}

static void testARelayThatMovedOnTheSameWifiIsFoundByBroadcast() {
  FakeNet net;
  net.relays["192.168.1.77"] = KEY;   // the Pi got a new address
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(l.ok && l.ip == "192.168.1.77" && !l.atConfigured);
  assert(net.countTo("255.255.255.255") == 1);
  bool swept = false;
  for (size_t i = 0; i < net.sentTo.size(); ++i) if (net.sentTo[i] == "192.168.1.100") swept = true;
  assert(!swept);
  printf("PASS a moved relay on the same WiFi is found by broadcast\n");
}

static void testBehindNatTheSweepOfTheStoredSubnetFindsIt() {
  FakeNet net;
  net.own = "192.168.2.40";            // the node sits on the extender's own network
  net.broadcastReaches = false;        // a broadcast stays on that network
  net.relays["192.168.1.57"] = KEY;    // the Pi, on the main router's network
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(l.ok && l.ip == "192.168.1.57" && !l.atConfigured);
  assert(net.sentTo.back() == "192.168.1.57");   // the sweep stopped at the answer
  bool inOwn = false, outside = false;
  for (size_t i = 0; i < net.sentTo.size(); ++i) {
    const std::string& t = net.sentTo[i];
    if (t == "255.255.255.255") continue;
    if (t.compare(0, 10, "192.168.2.") == 0) inOwn = true;
    if (t.compare(0, 10, "192.168.1.") != 0) outside = true;
  }
  assert(!inOwn && !outside);   // it swept the relay's /24 (from the stored host), not its own
  printf("PASS behind a NAT router the sweep of the stored subnet finds it\n");
}

static void testTheSweepCoversTheSubnetExceptItselfAndTheEnds() {
  FakeNet net;
  net.own = "192.168.1.50";
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);   // nobody answers
  assert(!l.ok);
  std::set<std::string> swept;
  for (size_t i = 0; i < net.sentTo.size(); ++i) swept.insert(net.sentTo[i]);
  assert(swept.count("192.168.1.1") && swept.count("192.168.1.254"));       // both ends of the usable range
  assert(!swept.count("192.168.1.0") && !swept.count("192.168.1.255"));     // not the network or broadcast address
  assert(!swept.count("192.168.1.50"));                                     // not itself
  assert(swept.size() == (size_t)(253 + 1));                                // 253 addresses plus the broadcast
  assert(net.sentTo.size() == (size_t)(2 + 2 + 253));                       // 2 unicast, 2 broadcasts, then the sweep
  printf("PASS the sweep covers the subnet except itself and the ends\n");
}

static void testANameThatNoLongerResolvesSweepsItsOwnSubnet() {
  FakeNet net;
  net.broadcastReaches = false;
  net.relays["192.168.1.33"] = KEY;
  Located l = RelayLocator(net).locate("raspberrypi.local", KEY);   // no DNS entry
  assert(l.ok && l.ip == "192.168.1.33" && !l.atConfigured);
  printf("PASS a name that no longer resolves sweeps its own subnet\n");
}

static void testAMachineWithoutTheKeyIsNotBelieved() {
  FakeNet net;
  net.broadcastReaches = false;
  net.relays["192.168.1.20"] = "not-the-key";   // answers first, and says yes
  net.relays["192.168.1.57"] = KEY;
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(l.ok && l.ip == "192.168.1.57");
  assert(net.countTo("192.168.1.20") >= 1 && net.countTo("192.168.1.57") >= 1);
  printf("PASS a machine without the key is not believed\n");
}

static void testOnlyARogueMeansNotFound() {
  FakeNet net;
  net.relays["192.168.1.20"] = "not-the-key";
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(!l.ok);   // never adopts an address that cannot prove it knows the key
  printf("PASS only a rogue means not found\n");
}

static void testAnOldAnswerIsNotBelieved() {
  FakeNet net;
  // An answer to some earlier probe is waiting in the socket; it is valid for ITS nonce, not this one.
  net.inbox.push_back(std::make_pair(std::string("192.168.1.66"), net.answer(KEY, "fedcba9876543210")));
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(!l.ok);
  printf("PASS an old answer is not believed\n");
}

static void testNoiseOnThePortCannotHoldItUp() {
  FakeNet net;
  for (int i = 0; i < 5000; ++i) net.inbox.push_back(std::make_pair(std::string("192.168.1.66"), std::string("noise")));
  Located l = RelayLocator(net).locate("192.168.1.9", KEY);
  assert(!l.ok);
  assert(net.now < 1000 + 20000);   // bounded time
  printf("PASS noise on the port cannot hold it up\n");
}

static void testNoKeyNoSearch() {
  FakeNet net;
  net.relays["192.168.1.9"] = KEY;
  Located l = RelayLocator(net).locate("192.168.1.9", "");
  assert(!l.ok && net.sentTo.empty());   // nothing is sent when no answer could be checked
  FakeNet bad;
  bad.nonce = "not a nonce";
  assert(!RelayLocator(bad).locate("192.168.1.9", KEY).ok && bad.sentTo.empty());
  printf("PASS no key, no search\n");
}

static void testAFailedSearchTakesABoundedFewSeconds() {
  FakeNet net;
  uint32_t start = net.now;
  RelayLocator(net).locate("192.168.1.9", KEY);
  uint32_t took = net.now - start;
  assert(took >= 2000 && took <= 5000);
  printf("PASS a failed search takes a bounded few seconds\n");
}

// ── When it looks, and what it changes (RelayFinder) ──

struct FakeStore : IAddressStore {
  std::vector<std::string> saved;
  bool ok = true;
  bool saveHost(const std::string& host) override {
    if (!ok) return false;
    saved.push_back(host);
    return true;
  }
};

static const uint32_t FIRST = 10000, OK = 60000, RMIN = 15000, RMAX = 300000;

static void testFinderWaitsForWifiAndTheFirstDelay() {
  FakeNet net; FakeStore store;
  net.relays["192.168.1.9"] = KEY;
  RelayFinder f(net, store, FIRST, OK, RMIN, RMAX);
  assert(f.tick(false, "192.168.1.9", KEY) == RelayFinder::Idle);
  net.now += 100000;
  assert(f.tick(false, "192.168.1.9", KEY) == RelayFinder::Idle);      // the wait does not run out offline
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Idle);       // the first delay starts now
  net.now += FIRST - 1;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Idle);
  net.now += 1;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Healthy);
  assert(store.saved.empty());   // a relay that answers where it was changes nothing
  printf("PASS finder waits for WiFi and the first delay\n");
}

static void testFinderChecksAgainAfterOkSecondsAndNeverRewritesAGoodAddress() {
  FakeNet net; FakeStore store;
  net.relays["192.168.1.9"] = KEY;
  RelayFinder f(net, store, FIRST, OK, RMIN, RMAX);
  f.tick(true, "192.168.1.9", KEY);
  net.now += FIRST;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Healthy);
  size_t sent = net.sentTo.size();
  net.now += OK - 1;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Idle && net.sentTo.size() == sent);
  net.now += 1;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Healthy);
  assert(store.saved.empty());
  printf("PASS finder checks again after OK seconds and never rewrites a good address\n");
}

static void testFinderMovesToTheNewAddressAndSaysRestart() {
  FakeNet net; FakeStore store;
  net.relays["192.168.1.77"] = KEY;
  RelayFinder f(net, store, FIRST, OK, RMIN, RMAX);
  f.tick(true, "192.168.1.9", KEY);
  net.now += FIRST;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Moved);
  assert(store.saved.size() == 1 && store.saved[0] == "192.168.1.77");
  assert(f.found() == "192.168.1.77");
  printf("PASS finder moves to the new address and says restart\n");
}

static void testFinderDoesNotMoveWhenItCannotSave() {
  FakeNet net; FakeStore store;
  store.ok = false;
  net.relays["192.168.1.77"] = KEY;
  RelayFinder f(net, store, FIRST, OK, RMIN, RMAX);
  f.tick(true, "192.168.1.9", KEY);
  net.now += FIRST;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::NotFound);   // no restart loop
  assert(f.failures() == 1);
  printf("PASS finder does not move when it cannot save\n");
}

static void testFinderBacksOffWhenNobodyAnswers() {
  FakeNet net; FakeStore store;
  RelayFinder f(net, store, FIRST, OK, RMIN, RMAX);
  f.tick(true, "192.168.1.9", KEY);
  net.now += FIRST;
  const uint32_t expected[] = {15000, 30000, 60000, 120000, 240000, 300000, 300000};
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::NotFound);
    net.now += expected[i] - 1;
    assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Idle);
    net.now += 1;
  }
  net.relays["192.168.1.9"] = KEY;   // the relay is back, where it was
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Healthy);
  assert(f.failures() == 0);         // the count starts over
  net.relays.clear();
  net.now += OK;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::NotFound);
  net.now += RMIN - 1;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Idle);   // the short wait again
  printf("PASS finder backs off when nobody answers and starts over when someone does\n");
}

static void testFinderSurvivesTheMillisWraparound() {
  FakeNet net; FakeStore store;
  net.now = 0xFFFFFFFFu - 5000;   // the counter wraps while it waits
  net.relays["192.168.1.9"] = KEY;
  RelayFinder f(net, store, FIRST, OK, RMIN, RMAX);
  f.tick(true, "192.168.1.9", KEY);
  net.now += FIRST - 1;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Idle);
  net.now += 1;
  assert(f.tick(true, "192.168.1.9", KEY) == RelayFinder::Healthy);
  printf("PASS finder survives the millis wraparound\n");
}

// ── The glue (relay_find.h) against the stub radio + a RAM flash store ──

struct RamBackend : espnet::WifiStoreBackend {
  std::string data;
  bool healthy = true;
  bool read(char* buf, size_t max) override {
    if (!healthy) return false;
    if (data.size() > max) return false;
    for (size_t i = 0; i < data.size(); ++i) buf[i] = data[i];
    return true;
  }
  bool write(const char* buf, size_t len) override {
    if (!healthy || len == 0) return false;
    data.assign(buf, len);
    return true;
  }
};

// The relay world the stub radio plays: `relays` (address -> key) answer a
// probe sent to them, or to the broadcast address when `broadcast` is true.
static std::map<std::string, std::string> g_relays;
static bool g_broadcast = true;

static void world(const std::map<std::string, std::string>& relays, bool broadcast = true) {
  fakeUdp() = FakeUdpState();
  WiFi.dns.clear();
  WiFi.dnsFailAll = false;
  WiFi.ownIp = "192.168.1.50";
  fakeNow = 5000;
  Stream::serialOut.clear();
  g_relays = relays;
  g_broadcast = broadcast;
  fakeUdp().hook = [](const std::string& ip, int, const std::string& payload) {
    if (payload.size() != 24 || payload.compare(0, 8, "ESPNET1?") != 0) return;
    std::string signedText = "ESPNET1!" + payload.substr(8);
    for (std::map<std::string, std::string>::const_iterator r = g_relays.begin(); r != g_relays.end(); ++r) {
      if (ip == r->first || (ip == "255.255.255.255" && g_broadcast)) {
        fakeUdp().inbox.push_back(std::make_pair(r->first, signedText + ":" + hmacSha256Hex(r->second, signedText)));
      }
    }
  };
}

static bool logged(const std::string& part) {
  return Stream::serialOut.find(part) != std::string::npos;
}

// A configured store + the live config the sketch would hold.
static void primeStore(RamBackend& be, espnet::RelayConfigStore& store, espnet::RelayConfig& cfg,
                       const char* host, int port, const char* key) {
  espnet::RelayConfig saved;
  saved.host = host;
  saved.port = port;
  saved.key = key;
  store.begin(&be);
  assert(store.save(saved));
  assert(store.load(cfg) && cfg.host == host);
}

// One tick that is due: arm it, let the first delay pass, tick again.
static bool dueTick(RelayFind& f, const std::string& host, bool relayDown = true) {
  f.tick(host, KEY, relayDown);
  fakeNow += 10UL * 1000UL;   // RelayFind's default first-check wait
  return f.tick(host, KEY, relayDown);
}

static void testNothingIsSentWhileTheRelayWorks() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.9", KEY}});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  RelayFind f(store, cfg);
  for (int i = 0; i < 5; ++i) {
    fakeNow += 120000;
    assert(!f.tick("192.168.1.9", KEY, false));
  }
  assert(fakeUdp().sent.empty() && fakeUdp().opens == 0);   // not one packet, not even a socket
  printf("PASS nothing is sent while the relay works\n");
}

static void testNothingIsSentWithoutWifi() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.9", KEY}});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  WiFi.wifiStatus = WL_DISCONNECTED;
  RelayFind f(store, cfg);
  assert(!dueTick(f, "192.168.1.9"));
  WiFi.wifiStatus = WL_CONNECTED;
  assert(fakeUdp().sent.empty());
  printf("PASS nothing is sent without WiFi\n");
}

static void testARelayThatAnswersWhereItWasChangesNothing() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.9", KEY}});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  RelayFind f(store, cfg);
  assert(!dueTick(f, "192.168.1.9"));
  assert(cfg.host == "192.168.1.9");                                   // untouched
  espnet::RelayConfig back;
  assert(store.load(back) && back.host == "192.168.1.9" && back.port == 7777 && back.key == KEY);
  assert(fakeUdp().sent.size() == 1);                                  // one probe
  assert(fakeUdp().sent[0].compare(0, 25, "192.168.1.9:8083 ESPNET1?") == 0);   // stored address, discovery port, probe tag
  assert(fakeUdp().sent[0].size() == (size_t)(16 + 1 + 24));           // ip:port + space + a 24-byte probe
  assert(fakeUdp().sent[0].substr(25) != "" && isNonce(fakeUdp().sent[0].substr(25)));  // a real nonce
  assert(fakeUdp().opens == 1 && fakeUdp().closes == 1 && !fakeUdp().open);     // socket opened and closed again
  printf("PASS a relay that answers where it was changes nothing\n");
}

static void testAMovedRelayIsStoredAndTheBoardRestarts() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.77", KEY}});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  RelayFind f(store, cfg);
  assert(dueTick(f, "192.168.1.9"));                                   // restart requested
  assert(cfg.host == "192.168.1.77" && cfg.port == 7777 && cfg.key == KEY);   // host changed, the rest rides along
  espnet::RelayConfig back;
  assert(store.load(back) && back.host == "192.168.1.77");             // it is in flash
  assert(logged("192.168.1.77") && logged("192.168.1.9"));             // the log says from where to where
  assert(!fakeUdp().open);                                             // socket closed
  printf("PASS a moved relay is stored and the board restarts\n");
}

static void testBehindNatOnlyTheSweepGetsThrough() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.200", KEY}}, /*broadcast=*/false);
  WiFi.ownIp = "192.168.2.40";   // the extender's own network
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  RelayFind f(store, cfg);
  assert(dueTick(f, "192.168.1.9"));
  assert(cfg.host == "192.168.1.200");                                 // found across the NAT
  printf("PASS behind a NAT router only the sweep gets through\n");
}

static void testAMachineThatDoesNotKnowTheKeyIsNeverStored() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.20", "not-the-key"}});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  RelayFind f(store, cfg);
  assert(!dueTick(f, "192.168.1.9"));
  assert(cfg.host == "192.168.1.9");                                   // untouched
  printf("PASS a machine that does not know the key is never stored\n");
}

static void testANameIsResolvedAndADeadNameStillLeadsToASearch() {
  // the stored host is a NAME that resolves to where the relay is: nothing to change
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.9", KEY}});
  primeStore(be, store, cfg, "myrelay.local", 7777, KEY);
  WiFi.dns["myrelay.local"] = "192.168.1.9";
  RelayFind f(store, cfg);
  assert(!dueTick(f, "myrelay.local"));
  assert(cfg.host == "myrelay.local" && WiFi.lastHostLookup == "myrelay.local");
  espnet::RelayConfig back;
  assert(store.load(back) && back.host == "myrelay.local");   // the name stays in flash

  // the name no longer resolves: the broadcast finds the relay and the name is replaced by its address
  RamBackend be2; espnet::RelayConfigStore store2; espnet::RelayConfig cfg2;
  world({{"192.168.1.77", KEY}});
  primeStore(be2, store2, cfg2, "myrelay.local", 7777, KEY);
  RelayFind f2(store2, cfg2);
  assert(dueTick(f2, "myrelay.local"));
  assert(cfg2.host == "192.168.1.77" && cfg2.port == 7777 && cfg2.key == KEY);
  printf("PASS a name is resolved and a dead name still leads to a search\n");
}

static void testNobodyAnsweringIsLoggedTheFirstTimeThenRarely() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  RelayFind f(store, cfg);
  f.tick("192.168.1.9", KEY, true);
  fakeNow += 10000;
  int warnings = 0;
  for (int i = 0; i < 13; ++i) {
    size_t before = Stream::serialOut.size();
    assert(!f.tick("192.168.1.9", KEY, true));
    if (Stream::serialOut.size() > before) ++warnings;
    fakeNow += 300000;
  }
  assert(warnings == 2);                                    // the 1st and the 12th of 13 tries
  assert(logged("relay not found") && logged("UDP 8083"));  // it says what to check
  assert(cfg.host == "192.168.1.9");                        // nothing stored
  printf("PASS nobody answering is logged the first time then rarely\n");
}

static void testAnAddressThatCannotBeSavedDoesNotRestartTheBoard() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.77", KEY}});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  be.healthy = false;   // the flash write will fail
  RelayFind f(store, cfg);
  assert(!dueTick(f, "192.168.1.9"));   // no restart (it would only loop)
  printf("PASS an address that cannot be saved does not restart the board\n");
}

static void testThePortAndTimingsComeFromTheConstructor() {
  RamBackend be; espnet::RelayConfigStore store; espnet::RelayConfig cfg;
  world({{"192.168.1.77", KEY}});
  primeStore(be, store, cfg, "192.168.1.9", 7777, KEY);
  RelayFind f(store, cfg, 9999, 5000, 60000, 15000, 300000);
  f.tick("192.168.1.9", KEY, true);
  fakeNow += 5000;   // the constructor's first-check wait, not the 10 s default
  assert(f.tick("192.168.1.9", KEY, true));
  assert(cfg.host == "192.168.1.77");
  assert(!fakeUdp().sent.empty());
  bool allOnPort = true;
  for (size_t i = 0; i < fakeUdp().sent.size(); ++i)
    if (fakeUdp().sent[i].find(":9999 ") == std::string::npos) allOnPort = false;
  assert(allOnPort);   // every probe went to the port the constructor gave
  assert(!fakeUdp().open);
  printf("PASS the port and timings come from the constructor\n");
}

int main() {
  testSha256KnownAnswers();
  testSha256PaddingEdges();
  testHmacRfc4231Vectors();
  testFleetMacVector();
  testReplyValidation();
  testProbeText();
  testIpv4Parsing();
  testFindsItWhereItWasWithOneProbe();
  testALostFirstProbeIsTriedAgain();
  testANameThatResolvesIsProbedFirst();
  testARelayThatMovedOnTheSameWifiIsFoundByBroadcast();
  testBehindNatTheSweepOfTheStoredSubnetFindsIt();
  testTheSweepCoversTheSubnetExceptItselfAndTheEnds();
  testANameThatNoLongerResolvesSweepsItsOwnSubnet();
  testAMachineWithoutTheKeyIsNotBelieved();
  testOnlyARogueMeansNotFound();
  testAnOldAnswerIsNotBelieved();
  testNoiseOnThePortCannotHoldItUp();
  testNoKeyNoSearch();
  testAFailedSearchTakesABoundedFewSeconds();
  testFinderWaitsForWifiAndTheFirstDelay();
  testFinderChecksAgainAfterOkSecondsAndNeverRewritesAGoodAddress();
  testFinderMovesToTheNewAddressAndSaysRestart();
  testFinderDoesNotMoveWhenItCannotSave();
  testFinderBacksOffWhenNobodyAnswers();
  testFinderSurvivesTheMillisWraparound();
  testNothingIsSentWhileTheRelayWorks();
  testNothingIsSentWithoutWifi();
  testARelayThatAnswersWhereItWasChangesNothing();
  testAMovedRelayIsStoredAndTheBoardRestarts();
  testBehindNatOnlyTheSweepGetsThrough();
  testAMachineThatDoesNotKnowTheKeyIsNeverStored();
  testANameIsResolvedAndADeadNameStillLeadsToASearch();
  testNobodyAnsweringIsLoggedTheFirstTimeThenRarely();
  testAnAddressThatCannotBeSavedDoesNotRestartTheBoard();
  testThePortAndTimingsComeFromTheConstructor();
  printf("tests/cpp/test_relay_find: all passed\n");
  return 0;
}
