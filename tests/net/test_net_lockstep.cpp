// lockstep.h: frame sets, delay, redundancy bookkeeping (D14, knowledge 22 section 4).
#include "check.h"
#include "lockstep.h"

using namespace don_net;

static NetPad pad_of(int v)
{
    NetPad p = neutral_pad();
    p.buttons = static_cast<uint16_t>(0x8000 + v);
    return p;
}

static void test_initial(int players, int delay)
{
    Lockstep ls(players, 0, delay);
    for (uint32_t f = 1; f <= static_cast<uint32_t>(delay); ++f)
    {
        CHECK(ls.confirmed(f));
        CHECK(ls.complete(f));
        CHECK_EQ(ls.missing(f), 0);
        for (int p = 0; p < players; ++p)
            CHECK(ls.confirmed_pad(f, p) == neutral_pad());
    }
    CHECK(!ls.confirmed(delay + 1));
    CHECK(!ls.complete(delay + 1));
    CHECK_EQ(ls.last_confirmed(), delay);
    CHECK_EQ(ls.missing(delay + 1), (1 << players) - 1);
}

static void test_submit_and_host()
{
    Lockstep ls(3, 0, 3);
    CHECK_EQ(ls.submit_local(10, pad_of(1)), 13);
    CHECK(!ls.complete(13));
    CHECK_EQ(ls.missing(13), 0b110);
    ls.record(2, 13, pad_of(2));
    CHECK_EQ(ls.missing(13), 0b010);
    ls.record(2, 13, pad_of(99)); // second value ignored
    ls.record(1, 13, pad_of(3));
    CHECK(ls.complete(13));
    CHECK_EQ(ls.missing(13), 0);
    ls.confirm(13, nullptr); // frames 4..12 are not confirmed yet: last_confirmed stays
    CHECK(ls.confirmed(13));
    CHECK_EQ(ls.last_confirmed(), 3);
    CHECK(ls.confirmed_pad(13, 0) == pad_of(1));
    CHECK(ls.confirmed_pad(13, 1) == pad_of(3));
    CHECK(ls.confirmed_pad(13, 2) == pad_of(2)); // the first value stayed
    ls.record(1, 13, pad_of(7)); // confirmed: ignored
    CHECK(ls.confirmed_pad(13, 1) == pad_of(3));
    // not complete: confirm(nullptr) does nothing
    ls.submit_local(11, pad_of(4));
    ls.confirm(14, nullptr);
    CHECK(!ls.confirmed(14));
    // out of range players ignored
    ls.record(3, 14, pad_of(1));
    ls.record(-1, 14, pad_of(1));
    CHECK_EQ(ls.missing(14), 0b110);
}

static void test_client()
{
    Lockstep ls(2, 1, 2);
    for (uint32_t f = 1; f <= 6; ++f)
        CHECK_EQ(ls.submit_local(f, pad_of(static_cast<int>(f))), f + 2);
    auto u = ls.unacked_local(100);
    CHECK_EQ(u.size(), 6);
    CHECK(u.size() == 6 && u.front().first == 3 && u.back().first == 8 && u.front().second == pad_of(1));
    CHECK_EQ(ls.unacked_local(2).size(), 2);
    CHECK(ls.unacked_local(2)[0].first == 3 && ls.unacked_local(2)[1].first == 4); // oldest first
    NetPad set[2] = {pad_of(50), pad_of(1)};
    ls.confirm(3, set);
    CHECK(ls.confirmed(3));
    CHECK(ls.confirmed_pad(3, 0) == pad_of(50));
    CHECK(ls.confirmed_pad(3, 1) == pad_of(1));
    CHECK_EQ(ls.last_confirmed(), 3);
    ls.ack_local(5);
    u = ls.unacked_local(100);
    CHECK_EQ(u.size(), 3);
    CHECK(u.size() == 3 && u[0].first == 6 && u[2].first == 8);
    ls.ack_local(4); // going back does nothing
    CHECK_EQ(ls.unacked_local(100).size(), 3);
    ls.ack_local(8);
    CHECK(ls.unacked_local(100).empty());
    // confirm of a frame already confirmed does not change it
    NetPad other[2] = {pad_of(9), pad_of(9)};
    ls.confirm(3, other);
    CHECK(ls.confirmed_pad(3, 0) == pad_of(50));
    // confirming out of order: last_confirmed is contiguous
    ls.confirm(5, set);
    CHECK_EQ(ls.last_confirmed(), 3);
    ls.confirm(4, set);
    CHECK_EQ(ls.last_confirmed(), 5);
}

static void test_confirmed_since()
{
    Lockstep ls(2, 0, 1);
    for (uint32_t f = 2; f <= 30; ++f)
    {
        ls.record(0, f, pad_of(1));
        ls.record(1, f, pad_of(2));
        ls.confirm(f, nullptr);
    }
    CHECK_EQ(ls.last_confirmed(), 30);
    auto v = ls.confirmed_since(0, 12);
    CHECK_EQ(v.size(), 12);
    CHECK(v.front() == 19 && v.back() == 30);
    v = ls.confirmed_since(25, 12);
    CHECK(v.size() == 5 && v.front() == 26 && v.back() == 30);
    CHECK(ls.confirmed_since(30, 12).empty());
    CHECK(ls.confirmed_since(99, 12).empty());
    CHECK_EQ(ls.confirmed_since(28, 1).size(), 1);
    CHECK_EQ(ls.confirmed_since(28, 1)[0], 30);
}

static void test_forget()
{
    Lockstep ls(2, 0, 2);
    for (uint32_t f = 3; f <= 20; ++f)
    {
        ls.record(0, f, pad_of(1));
        ls.record(1, f, pad_of(2));
        ls.confirm(f, nullptr);
    }
    ls.forget_before(10);
    CHECK(!ls.confirmed(5));
    CHECK(ls.confirmed(10));
    CHECK(ls.confirmed_pad(15, 1) == pad_of(2));
    CHECK_EQ(ls.last_confirmed(), 20);
    ls.record(0, 5, pad_of(7)); // forgotten: ignored, creates nothing
    CHECK(!ls.complete(5));
    ls.record(0, 21, pad_of(1));
    ls.record(1, 21, pad_of(1));
    ls.confirm(21, nullptr);
    CHECK_EQ(ls.last_confirmed(), 21);
    // frames <= delay stay confirmed
    CHECK(ls.confirmed(1) && ls.confirmed(2));
}

int main()
{
    test_initial(2, 1);
    test_initial(4, 1);
    test_initial(4, 10);
    test_initial(2, 3);
    test_submit_and_host();
    test_client();
    test_confirmed_since();
    test_forget();

    // 4 players, delay 10: a full run in both roles agrees.
    for (int delay : {1, 10})
    {
        Lockstep host(4, 0, delay);
        Lockstep client(4, 2, delay);
        for (uint32_t f = 1; f <= 200; ++f)
        {
            for (int p = 0; p < 4; ++p)
                host.record(p, f + delay, pad_of(p * 1000 + static_cast<int>(f)));
            CHECK(host.complete(f + delay));
            host.confirm(f + delay, nullptr);
            NetPad set[4];
            for (int p = 0; p < 4; ++p)
                set[p] = host.confirmed_pad(f + delay, p);
            client.confirm(f + delay, set);
            if (f > 100)
            {
                host.forget_before(f - 50);
                client.forget_before(f - 50);
            }
        }
        CHECK_EQ(host.last_confirmed(), 200 + delay);
        CHECK_EQ(client.last_confirmed(), 200 + delay);
        CHECK(client.confirmed_pad(200 + delay, 3) == pad_of(3000 + 200));
    }
    return check_result("test_net_lockstep");
}
