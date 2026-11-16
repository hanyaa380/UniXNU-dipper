/* SPDX-License-Identifier: MIT */
#include "kernel/QcomUsbControl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t
frame(char *out, size_t capacity, const char *session, const char *seq,
      const char *verb)
{
    char prefix[96];
    int prefix_bytes = snprintf(prefix, sizeof(prefix), "SXNU1 %s %s %s", session, seq, verb);
    uint32_t crc;
    int bytes;
    assert(prefix_bytes > 0 && (size_t)prefix_bytes < sizeof(prefix));
    crc = sxnu_control_crc32(prefix, (size_t)prefix_bytes);
    bytes = snprintf(out, capacity, "%s %08x\n", prefix, crc);
    assert(bytes > 0 && (size_t)bytes < capacity);
    return (size_t)bytes;
}

static int
feed(struct sxnu_control_parser *parser, const char *bytes, size_t count,
     char *line, size_t *line_bytes)
{
    size_t i;
    for (i = 0; i < count; i++) {
        int result = sxnu_control_parser_feed(parser, (uint8_t)bytes[i], line,
                                               SXNU_CONTROL_FRAME_BYTES, line_bytes);
        if (result) return result;
    }
    return 0;
}

static void
test_known_crc_and_fragmented_frames(void)
{
    static const char hello[] =
        "SXNU1 0000000000000000 00000000 HELLO e5b60eb1\n";
    static const char ping[] =
        "SXNU1 1122334455667788 00000001 PING 13527328\n";
    static const char status[] =
        "SXNU1 1122334455667788 00000002 STATUS 364e0ddf\n";
    struct sxnu_control_parser parser = {0};
    struct sxnu_control_frame parsed;
    char line[SXNU_CONTROL_FRAME_BYTES];
    size_t line_bytes = 0, i;
    char combined[128];

    assert(sxnu_control_crc32("123456789", 9) == 0xcbf43926u);
    for (i = 1; i < sizeof(hello) - 1; i++) assert(feed(&parser, hello + i - 1, 1, line, &line_bytes) == 0);
    assert(feed(&parser, hello + sizeof(hello) - 2, 1, line, &line_bytes) == 1);
    assert(sxnu_control_parse(line, line_bytes, &parsed));
    assert(parsed.verb == SXNU_CONTROL_VERB_HELLO && parsed.session == 0 && parsed.seq == 0);

    memcpy(combined, ping, sizeof(ping) - 1);
    memcpy(combined + sizeof(ping) - 1, status, sizeof(status) - 1);
    for (i = 0; i < sizeof(ping) + sizeof(status) - 2; i++) {
        int result = sxnu_control_parser_feed(&parser, (uint8_t)combined[i], line,
                                               sizeof(line), &line_bytes);
        if (result == 1) {
            assert(sxnu_control_parse(line, line_bytes, &parsed));
            assert((i + 1 <= sizeof(ping) - 1 &&
                    parsed.verb == SXNU_CONTROL_VERB_PING) ||
                   (i + 1 > sizeof(ping) - 1 &&
                    parsed.verb == SXNU_CONTROL_VERB_STATUS));
        } else {
            assert(result == 0);
        }
    }
    puts("PASS: known CRC, byte-fragmented frame and multiple frames in one budget chunk");
}

static void
test_malformed_recovery(void)
{
    struct sxnu_control_parser parser = {0};
    struct sxnu_control_frame parsed;
    char line[SXNU_CONTROL_FRAME_BYTES];
    size_t line_bytes = 0, i;
    char valid_ping[96], bad_crc[96], unknown[96];
    size_t ping_bytes = frame(valid_ping, sizeof(valid_ping),
                              "1122334455667788", "00000001", "PING");
    char junk[SXNU_CONTROL_FRAME_BYTES + 8];

    memcpy(bad_crc, valid_ping, ping_bytes);
    bad_crc[ping_bytes - 2] = bad_crc[ping_bytes - 2] == '0' ? '1' : '0';
    assert(feed(&parser, bad_crc, ping_bytes, line, &line_bytes) == 1);
    assert(!sxnu_control_parse(line, line_bytes, &parsed));

    frame(unknown, sizeof(unknown), "1122334455667788", "00000001", "EVIL");
    assert(feed(&parser, unknown, strlen(unknown), line, &line_bytes) == 1);
    assert(!sxnu_control_parse(line, line_bytes, &parsed));

    memset(junk, 'A', sizeof(junk) - 1); junk[sizeof(junk) - 1] = '\n';
    for (i = 0; i < sizeof(junk) - 2; i++) assert(feed(&parser, junk + i, 1, line, &line_bytes) == 0);
    assert(feed(&parser, junk + sizeof(junk) - 2, 1, line, &line_bytes) == 0);
    assert(feed(&parser, junk + sizeof(junk) - 1, 1, line, &line_bytes) == -1);
    assert(parser.overlong == 1);
    assert(feed(&parser, valid_ping, ping_bytes, line, &line_bytes) == 1);
    assert(sxnu_control_parse(line, line_bytes, &parsed));
    assert(parsed.verb == SXNU_CONTROL_VERB_PING);
    puts("PASS: bad CRC and unknown verbs are inert; overlong lines recover only at newline");
}

static void
test_session_sequence_and_no_action(void)
{
    struct sxnu_control_state state;
    struct sxnu_control_frame frames[8];
    char text[96];
    size_t bytes;
    unsigned int i;
    const char *seqs[] = {"00000001", "00000002", "00000003", "00000004"};

    sxnu_control_state_reset(&state, UINT64_C(0x1122334455667788), 7);
    for (i = 0; i < 4; i++) {
        bytes = frame(text, sizeof(text), "1122334455667788", seqs[i], "PING");
        assert(sxnu_control_parse(text, bytes - 1, &frames[i]));
    }
    bytes = frame(text, sizeof(text), "0000000000000000", "00000000", "HELLO");
    assert(sxnu_control_parse(text, bytes - 1, &frames[4]));
    bytes = frame(text, sizeof(text), "0000000000000000", "00000001", "PING");
    assert(sxnu_control_parse(text, bytes - 1, &frames[5]));
    bytes = frame(text, sizeof(text), "8877665544332211", "00000001", "PING");
    assert(sxnu_control_parse(text, bytes - 1, &frames[6]));
    bytes = frame(text, sizeof(text), "1122334455667788", "00000001", "PING");
    assert(sxnu_control_parse(text, bytes - 1, &frames[7]));

    assert(sxnu_control_accept(&state, &frames[4]) == SXNU_CONTROL_VERB_HELLO);
    assert(sxnu_control_accept(&state, &frames[5]) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_accept(&state, &frames[6]) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_accept(&state, &frames[7]) == SXNU_CONTROL_VERB_PING);
    assert(sxnu_control_accept(&state, &frames[2]) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_commit(&state, &frames[7], SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == 1);
    assert(sxnu_control_accept(&state, &frames[7]) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_accept(&state, &frames[1]) == SXNU_CONTROL_VERB_PING);
    assert(sxnu_control_commit(&state, &frames[1], SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == 2);
    assert(sxnu_control_accept(&state, &frames[3]) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_accept(&state, &frames[2]) == SXNU_CONTROL_VERB_PING);
    assert(sxnu_control_commit(&state, &frames[2], SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == 3);
    assert(sxnu_control_accept(&state, &frames[2]) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_accept(&state, &frames[3]) == SXNU_CONTROL_VERB_PING);
    assert(sxnu_control_commit(&state, &frames[3], SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == 4);
    assert(sxnu_control_accept(&state, &frames[4]) == SXNU_CONTROL_VERB_HELLO);
    puts("PASS: HELLO is read-only; session, exact-next-sequence, gap and replay gates have no action");
}

static void
test_exact_length_boundaries(void)
{
    char text[96];
    size_t bytes = frame(text, sizeof(text), "1122334455667788", "00000001", "PING");
    size_t content = bytes - 1u;
    struct sxnu_control_frame sentinel, parsed;
    size_t length;

    memset(&sentinel, 0xa5, sizeof(sentinel));
    for (length = 0; length < content; length++) {
        char *exact = malloc(length ? length : 1u);
        assert(exact);
        memcpy(exact, text, length);
        parsed = sentinel;
        /* The heap allocation is exactly the logical frame length: ASan must
         * catch any parser read beyond `length` (there is no trailing NUL). */
        assert(!sxnu_control_parse(exact, length, &parsed));
        assert(!memcmp(&parsed, &sentinel, sizeof(parsed)));
        free(exact);
    }

    {
        char *exact = malloc(content);
        assert(exact);
        memcpy(exact, text, content);
        assert(sxnu_control_parse(exact, content, &parsed));
        assert(parsed.session == UINT64_C(0x1122334455667788));
        assert(parsed.seq == 1u && parsed.verb == SXNU_CONTROL_VERB_PING);
        free(exact);
    }
    puts("PASS: every exact-length truncation, including session/seq fields, is inert without a trailing NUL");
}

static void
test_sequence_rollover_and_unknown_enum(void)
{
    struct sxnu_control_state state;
    struct sxnu_control_frame seq_zero, seq_one, seq_max, unknown;
    char text[96];
    size_t bytes;
    uint32_t last_seq;

    memset(&unknown, 0, sizeof(unknown));
    unknown.session = UINT64_C(0x1122334455667788);
    unknown.seq = 1u;
    unknown.verb = (enum sxnu_control_verb)99;
    sxnu_control_state_reset(&state, unknown.session, 3u);
    assert(sxnu_control_accept(&state, &unknown) == SXNU_CONTROL_VERB_INVALID);
    assert(!sxnu_control_commit(&state, &unknown, SXNU_CONTROL_VERB_PING));
    assert(!sxnu_control_commit(&state, &unknown, (enum sxnu_control_verb)99));
    assert(state.session == unknown.session && state.last_seq == 0u);

    bytes = frame(text, sizeof(text), "1122334455667788", "00000000", "PING");
    assert(sxnu_control_parse(text, bytes - 1u, &seq_zero));
    assert(sxnu_control_accept(&state, &seq_zero) == SXNU_CONTROL_VERB_INVALID);
    assert(!sxnu_control_commit(&state, &seq_zero, SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == 0u);

    bytes = frame(text, sizeof(text), "1122334455667788", "00000001", "PING");
    assert(sxnu_control_parse(text, bytes - 1u, &seq_one));
    bytes = frame(text, sizeof(text), "1122334455667788", "ffffffff", "PING");
    assert(sxnu_control_parse(text, bytes - 1u, &seq_max));

    state.last_seq = UINT32_MAX;
    last_seq = state.last_seq;
    assert(sxnu_control_accept(&state, &seq_one) == SXNU_CONTROL_VERB_INVALID);
    assert(!sxnu_control_commit(&state, &seq_one, SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == last_seq);

    state.last_seq = UINT32_MAX - 1u;
    assert(sxnu_control_accept(&state, &seq_max) == SXNU_CONTROL_VERB_PING);
    assert(sxnu_control_commit(&state, &seq_max, SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == UINT32_MAX);
    assert(sxnu_control_accept(&state, &seq_zero) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_accept(&state, &seq_one) == SXNU_CONTROL_VERB_INVALID);
    assert(!sxnu_control_commit(&state, &seq_one, SXNU_CONTROL_VERB_PING));
    assert(state.last_seq == UINT32_MAX);
    puts("PASS: non-HELLO seq zero, UINT32_MAX rollover and unknown enum are rejected without actions");
}

static void
test_reply_and_budget(void)
{
    struct sxnu_control_writer writer;
    char reply[SXNU_CONTROL_REPLY_BYTES];
    char small[48];
    size_t bytes, crc_offset;
    uint64_t parsed_crc;

    assert(SXNU_CONTROL_FRAME_BYTES == 128u && SXNU_CONTROL_REPLY_BYTES == 512u);
    assert(SXNU_CONTROL_WORK_BYTES == 256u);
    sxnu_control_writer_init(&writer, reply, sizeof(reply));
    assert(sxnu_control_reply_start(&writer, UINT64_C(0x1122334455667788), 3u,
                                    SXNU_CONTROL_VERB_STATUS, "OK"));
    assert(sxnu_control_writer_text(&writer, " usb=3 er=0 rx=0 rxo=0 tx=0 bp=0 poll=9 gen=2 rej=0"));
    bytes = sxnu_control_reply_finish(&writer);
    assert(bytes && bytes < sizeof(reply) && reply[bytes - 1] == '\n');
    crc_offset = bytes - 10u;
    assert(reply[crc_offset] == ' ');
    assert(sxnu_control_parse_hex((const uint8_t *)reply + crc_offset + 1, 8, &parsed_crc));
    assert((uint32_t)parsed_crc == sxnu_control_crc32(reply, crc_offset));

    sxnu_control_writer_init(&writer, small, sizeof(small));
    assert(sxnu_control_reply_start(&writer, 1, 2, SXNU_CONTROL_VERB_PING, "OK"));
    assert(!sxnu_control_writer_text(&writer,
                                     " this status deliberately exceeds the small reply buffer"));
    assert(writer.failed);
    assert(sxnu_control_reply_finish(&writer) == 0);
    puts("PASS: 512-byte status reply is CRC-correct; fixed frame/work budgets and truncation are enforced");
}

int main(void)
{
    test_known_crc_and_fragmented_frames();
    test_malformed_recovery();
    test_session_sequence_and_no_action();
    test_exact_length_boundaries();
    test_sequence_rollover_and_unknown_enum();
    test_reply_and_budget();
    return 0;
}
