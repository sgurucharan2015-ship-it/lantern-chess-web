// Lantern 16.4 - a UCI chess engine
// Classical evaluation (Texel-tuned on self-play data) + optional NNUE (PSN1 format, any hidden size).
// Search: PVS, aspiration windows, lockless 2-way bucketed TT, Lazy SMP, null move, ProbCut, RFP, razoring,
// LMR/LMP/futility/SEE/history pruning, singular + double extensions, correction history, pondering.
// Built-in tools: perft, bench, eval, datagen (self-play training data), texel (eval tuner).
// Bitboards + magic sliders, PVS alpha-beta with TT, null move, LMR, LMP,
// futility/SEE pruning, killer/history/countermove ordering, tapered eval.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <random>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif
using namespace std;
typedef uint64_t U64;

#define BIT(s) (1ULL << (s))
#ifdef _MSC_VER
static inline int lsb(U64 b) { unsigned long i; _BitScanForward64(&i, b); return (int)i; }
static inline int popc(U64 b) { return (int)__popcnt64(b); }
static inline U64 mulhi64(U64 a, U64 b) { return __umulh(a, b); }
#else
static inline int lsb(U64 b) { return __builtin_ctzll(b); }
static inline int popc(U64 b) { return __builtin_popcountll(b); }
static inline U64 mulhi64(U64 a, U64 b) { return (U64)(((unsigned __int128)a * b) >> 64); }
#endif
static inline int poplsb(U64 &b) { int s = lsb(b); b &= b - 1; return s; }

enum { WHITE, BLACK };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
const int EMPTY = 12;
enum { F_NONE = 0, F_EP = 1, F_CASTLE = 2, F_DOUBLE = 3 };
static inline int mFrom(int m) { return m & 63; }
static inline int mTo(int m) { return (m >> 6) & 63; }
static inline int mPromo(int m) { return (m >> 12) & 7; }
static inline int mFlag(int m) { return (m >> 15) & 3; }
static inline int mkMove(int f, int t, int pr = 0, int fl = 0) { return f | (t << 6) | (pr << 12) | (fl << 15); }

const U64 FILE_A = 0x0101010101010101ULL, FILE_H = FILE_A << 7;
const U64 RANK_1 = 0xFFULL, RANK_3 = RANK_1 << 16, RANK_6 = RANK_1 << 40, RANK_8 = RANK_1 << 56;

// ---------------------------------------------------------------- attacks
U64 pawnAtt[2][64], knightAtt[64], kingAtt[64];
struct Magic { U64 mask, magic; U64 *att; int shift; };
Magic RM[64], BM[64];
U64 RT[102400], BT[5248];
static U64 rngState = 0x9E3779B97F4A7C15ULL;
static U64 rnd() { rngState ^= rngState >> 12; rngState ^= rngState << 25; rngState ^= rngState >> 27; return rngState * 2685821657736338717ULL; }

static const int RD[4][2] = {{1,0},{-1,0},{0,1},{0,-1}}, BD[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
static U64 slideAtt(int sq, U64 occ, bool rook) {
    const int (*d)[2] = rook ? RD : BD; U64 a = 0;
    for (int k = 0; k < 4; k++) {
        int r = sq / 8 + d[k][0], f = sq % 8 + d[k][1];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) { int s = r * 8 + f; a |= BIT(s); if (occ & BIT(s)) break; r += d[k][0]; f += d[k][1]; }
    }
    return a;
}
static U64 slideMask(int sq, bool rook) {
    const int (*d)[2] = rook ? RD : BD; U64 a = 0;
    for (int k = 0; k < 4; k++) {
        int r = sq / 8 + d[k][0], f = sq % 8 + d[k][1];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int nr = r + d[k][0], nf = f + d[k][1];
            if (!(nr >= 0 && nr < 8 && nf >= 0 && nf < 8)) break;
            a |= BIT(r * 8 + f); r = nr; f = nf;
        }
    }
    return a;
}
static void initMagics(bool rook) {
    U64 *table = rook ? RT : BT; Magic *M = rook ? RM : BM; size_t off = 0;
    for (int sq = 0; sq < 64; sq++) {
        Magic &m = M[sq];
        m.mask = slideMask(sq, rook); int n = popc(m.mask); m.shift = 64 - n; m.att = table + off;
        int size = 1 << n;
        vector<U64> occs(size), atts(size); vector<int> epoch(size, 0);
        U64 b = 0; int i = 0;
        do { occs[i] = b; atts[i] = slideAtt(sq, b, rook); i++; b = (b - m.mask) & m.mask; } while (b);
        int cnt = 0;
        while (true) {
            U64 mg = rnd() & rnd() & rnd();
            if (popc((m.mask * mg) >> 56) < 6) continue;
            cnt++; bool ok = true;
            for (int j = 0; j < size; j++) {
                unsigned idx = (unsigned)((occs[j] * mg) >> m.shift);
                if (epoch[idx] < cnt) { epoch[idx] = cnt; m.att[idx] = atts[j]; }
                else if (m.att[idx] != atts[j]) { ok = false; break; }
            }
            if (ok) { m.magic = mg; break; }
        }
        off += size;
    }
}
static inline U64 rookA(int sq, U64 occ) { const Magic &m = RM[sq]; return m.att[((occ & m.mask) * m.magic) >> m.shift]; }
static inline U64 bishopA(int sq, U64 occ) { const Magic &m = BM[sq]; return m.att[((occ & m.mask) * m.magic) >> m.shift]; }

static void initAttacks() {
    for (int sq = 0; sq < 64; sq++) {
        int r = sq / 8, f = sq % 8;
        auto add = [&](U64 &b, int dr, int df) { int nr = r + dr, nf = f + df; if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) b |= BIT(nr * 8 + nf); };
        add(pawnAtt[WHITE][sq], 1, -1); add(pawnAtt[WHITE][sq], 1, 1);
        add(pawnAtt[BLACK][sq], -1, -1); add(pawnAtt[BLACK][sq], -1, 1);
        int kn[8][2] = {{1,2},{2,1},{-1,2},{-2,1},{1,-2},{2,-1},{-1,-2},{-2,-1}};
        for (auto &k : kn) add(knightAtt[sq], k[0], k[1]);
        for (int dr = -1; dr <= 1; dr++) for (int df = -1; df <= 1; df++) if (dr || df) add(kingAtt[sq], dr, df);
    }
    initMagics(true); initMagics(false);
}

// ---------------------------------------------------------------- zobrist
U64 zPiece[12][64], zSide, zCastle[16], zEp[8];
int castleMask[64];
static void initZobrist() {
    U64 s = 0x1234567ULL; auto r = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    for (int s1 = 0; s1 < 50; s1++) r();
    for (auto &a : zPiece) for (auto &b : a) b = r();
    zSide = r(); for (auto &c : zCastle) c = r(); for (auto &e : zEp) e = r();
    for (int i = 0; i < 64; i++) castleMask[i] = 15;
    castleMask[4] = 12; castleMask[0] = 13; castleMask[7] = 14;
    castleMask[60] = 3; castleMask[56] = 7; castleMask[63] = 11;
}

// ---------------------------------------------------------------- position
struct Pos {
    U64 pc[12], col[2], all; int8_t bd[64]; int side, ep, castle, fifty; U64 key, pkey, npkey[2];
    int nd; int8_t dp[8], ds[8], da[8];   // pieces changed by the last make(): piece, square, +1 add / -1 remove
    inline void dirty(int p, int s, int a) { if (nd < 8) { dp[nd] = (int8_t)p; ds[nd] = (int8_t)s; da[nd] = (int8_t)a; nd++; } }
    void clear() { memset(this, 0, sizeof(*this)); for (int i = 0; i < 64; i++) bd[i] = EMPTY; ep = -1; }
    inline void add(int p, int s) { pc[p] |= BIT(s); col[p / 6] |= BIT(s); all |= BIT(s); bd[s] = p; key ^= zPiece[p][s]; if (p % 6 == PAWN) pkey ^= zPiece[p][s]; else npkey[p / 6] ^= zPiece[p][s]; dirty(p, s, 1); }
    inline void rem(int p, int s) { pc[p] ^= BIT(s); col[p / 6] ^= BIT(s); all ^= BIT(s); bd[s] = EMPTY; key ^= zPiece[p][s]; if (p % 6 == PAWN) pkey ^= zPiece[p][s]; else npkey[p / 6] ^= zPiece[p][s]; dirty(p, s, -1); }
    inline void mov(int p, int f, int t) { U64 x = BIT(f) | BIT(t); pc[p] ^= x; col[p / 6] ^= x; all ^= x; bd[f] = EMPTY; bd[t] = p; key ^= zPiece[p][f] ^ zPiece[p][t]; if (p % 6 == PAWN) pkey ^= zPiece[p][f] ^ zPiece[p][t]; else npkey[p / 6] ^= zPiece[p][f] ^ zPiece[p][t]; dirty(p, f, -1); dirty(p, t, 1); }
    inline int kingSq(int c) const { return lsb(pc[c * 6 + KING]); }
    bool attacked(int sq, int by) const {
        if (pawnAtt[by ^ 1][sq] & pc[by * 6 + PAWN]) return true;
        if (knightAtt[sq] & pc[by * 6 + KNIGHT]) return true;
        if (kingAtt[sq] & pc[by * 6 + KING]) return true;
        U64 bq = pc[by * 6 + BISHOP] | pc[by * 6 + QUEEN];
        if (bq && (bishopA(sq, all) & bq)) return true;
        U64 rq = pc[by * 6 + ROOK] | pc[by * 6 + QUEEN];
        if (rq && (rookA(sq, all) & rq)) return true;
        return false;
    }
    inline bool inCheck() const { return attacked(kingSq(side), side ^ 1); }
    U64 attackersTo(int sq, U64 occ) const {
        return (pawnAtt[BLACK][sq] & pc[PAWN]) | (pawnAtt[WHITE][sq] & pc[6 + PAWN])
             | (knightAtt[sq] & (pc[KNIGHT] | pc[6 + KNIGHT]))
             | (kingAtt[sq] & (pc[KING] | pc[6 + KING]))
             | (bishopA(sq, occ) & (pc[BISHOP] | pc[QUEEN] | pc[6 + BISHOP] | pc[6 + QUEEN]))
             | (rookA(sq, occ) & (pc[ROOK] | pc[QUEEN] | pc[6 + ROOK] | pc[6 + QUEEN]));
    }
    bool make(int m) {
        int from = mFrom(m), to = mTo(m), promo = mPromo(m), flag = mFlag(m);
        int p = bd[from], cap = bd[to], us = side;
        nd = 0;
        if (ep != -1) { key ^= zEp[ep & 7]; ep = -1; }
        fifty++;
        if (cap != EMPTY) { rem(cap, to); fifty = 0; }
        mov(p, from, to);
        if (p % 6 == PAWN) {
            fifty = 0;
            if (flag == F_EP) { int cs = to ^ 8; rem(bd[cs], cs); }
            else if (flag == F_DOUBLE) { ep = (from + to) / 2; key ^= zEp[ep & 7]; }
            else if (promo) { rem(p, to); add(us * 6 + promo, to); }
        } else if (flag == F_CASTLE) {
            switch (to) {
                case 6: mov(ROOK, 7, 5); break;
                case 2: mov(ROOK, 0, 3); break;
                case 62: mov(6 + ROOK, 63, 61); break;
                case 58: mov(6 + ROOK, 56, 59); break;
            }
        }
        key ^= zCastle[castle]; castle &= castleMask[from] & castleMask[to]; key ^= zCastle[castle];
        side ^= 1; key ^= zSide;
        return !attacked(kingSq(us), side);
    }
    void makeNull() {
        nd = 0;
        if (ep != -1) { key ^= zEp[ep & 7]; ep = -1; }
        side ^= 1; key ^= zSide; fifty = 0;
    }
    void setFen(const string &fen) {
        clear();
        istringstream ss(fen); string b, s, c, e; int h = 0, f = 1;
        ss >> b >> s >> c >> e >> h >> f;
        int r = 7, fl = 0; const string P = "PNBRQKpnbrqk";
        for (char ch : b) {
            if (ch == '/') { r--; fl = 0; }
            else if (isdigit((unsigned char)ch)) fl += ch - '0';
            else { size_t i = P.find(ch); if (i != string::npos && r >= 0 && fl < 8) add((int)i, r * 8 + fl); fl++; }
        }
        side = (s == "b") ? BLACK : WHITE; if (side) key ^= zSide;
        castle = 0;
        for (char ch : c) { if (ch == 'K') castle |= 1; if (ch == 'Q') castle |= 2; if (ch == 'k') castle |= 4; if (ch == 'q') castle |= 8; }
        key ^= zCastle[castle];
        if (e.size() == 2 && e != "-") { ep = (e[1] - '1') * 8 + (e[0] - 'a'); key ^= zEp[ep & 7]; }
        fifty = h;
    }
};

static string moveStr(int m) {
    if (!m) return "0000";
    string s; s += char('a' + mFrom(m) % 8); s += char('1' + mFrom(m) / 8); s += char('a' + mTo(m) % 8); s += char('1' + mTo(m) / 8);
    if (mPromo(m)) s += " nbrq"[mPromo(m)];
    return s;
}

// ---------------------------------------------------------------- movegen
struct MoveList { int mv[256]; int sc[256]; int n = 0; inline void add(int m) { mv[n++] = m; } };

static void genMoves(const Pos &p, MoveList &ml, bool noisy) {
    int us = p.side, them = us ^ 1;
    U64 own = p.col[us], opp = p.col[them], empty = ~p.all;
    U64 pawns = p.pc[us * 6 + PAWN];
    U64 promoRank = us == WHITE ? RANK_8 : RANK_1;
    int fwd = us == WHITE ? 8 : -8;
    U64 p1 = us == WHITE ? (pawns << 8) & empty : (pawns >> 8) & empty;
    U64 b = p1 & promoRank;
    while (b) {
        int t = poplsb(b), f = t - fwd;
        ml.add(mkMove(f, t, QUEEN));
        if (!noisy) { ml.add(mkMove(f, t, KNIGHT)); ml.add(mkMove(f, t, ROOK)); ml.add(mkMove(f, t, BISHOP)); }
    }
    if (!noisy) {
        b = p1 & ~promoRank;
        while (b) { int t = poplsb(b); ml.add(mkMove(t - fwd, t)); }
        U64 p2 = us == WHITE ? ((p1 & RANK_3) << 8) & empty : ((p1 & RANK_6) >> 8) & empty;
        while (p2) { int t = poplsb(p2); ml.add(mkMove(t - 2 * fwd, t, 0, F_DOUBLE)); }
    }
    b = pawns;
    while (b) {
        int f = poplsb(b); U64 a = pawnAtt[us][f] & opp;
        while (a) {
            int t = poplsb(a);
            if (BIT(t) & promoRank) {
                ml.add(mkMove(f, t, QUEEN));
                if (!noisy) { ml.add(mkMove(f, t, KNIGHT)); ml.add(mkMove(f, t, ROOK)); ml.add(mkMove(f, t, BISHOP)); }
            } else ml.add(mkMove(f, t));
        }
    }
    if (p.ep != -1) { U64 a = pawnAtt[them][p.ep] & pawns; while (a) { int f = poplsb(a); ml.add(mkMove(f, p.ep, 0, F_EP)); } }
    U64 tgt = noisy ? opp : ~own;
    for (int pt = KNIGHT; pt <= KING; pt++) {
        U64 bb = p.pc[us * 6 + pt];
        while (bb) {
            int f = poplsb(bb); U64 a;
            switch (pt) {
                case KNIGHT: a = knightAtt[f]; break;
                case BISHOP: a = bishopA(f, p.all); break;
                case ROOK: a = rookA(f, p.all); break;
                case QUEEN: a = bishopA(f, p.all) | rookA(f, p.all); break;
                default: a = kingAtt[f];
            }
            a &= tgt;
            while (a) ml.add(mkMove(f, poplsb(a)));
        }
    }
    if (!noisy) {
        if (us == WHITE) {
            if ((p.castle & 1) && !(p.all & (BIT(5) | BIT(6))) && !p.attacked(4, them) && !p.attacked(5, them)) ml.add(mkMove(4, 6, 0, F_CASTLE));
            if ((p.castle & 2) && !(p.all & (BIT(1) | BIT(2) | BIT(3))) && !p.attacked(4, them) && !p.attacked(3, them)) ml.add(mkMove(4, 2, 0, F_CASTLE));
        } else {
            if ((p.castle & 4) && !(p.all & (BIT(61) | BIT(62))) && !p.attacked(60, them) && !p.attacked(61, them)) ml.add(mkMove(60, 62, 0, F_CASTLE));
            if ((p.castle & 8) && !(p.all & (BIT(57) | BIT(58) | BIT(59))) && !p.attacked(60, them) && !p.attacked(59, them)) ml.add(mkMove(60, 58, 0, F_CASTLE));
        }
    }
}

static U64 perft(const Pos &p, int d) {
    MoveList ml; genMoves(p, ml, false); U64 n = 0;
    for (int i = 0; i < ml.n; i++) { Pos np = p; if (np.make(ml.mv[i])) n += d <= 1 ? 1 : perft(np, d - 1); }
    return n;
}

// ---------------------------------------------------------------- evaluation
// Starting values for the tuner: plain material, flat piece-square tables. Every evaluation number used
// in play comes from Lantern's own Texel tuning on its own self-play games (the TUNED table below).
static const int mgVal[6] = {100, 300, 300, 500, 900, 0}, egVal[6] = {100, 300, 300, 500, 900, 0};
static const int phaseInc[6] = {0, 1, 1, 2, 4, 0};

static const int passedMg[8] = {0, 0, 0, 5, 12, 25, 40, 0};
static const int passedEg[8] = {0, 5, 8, 15, 30, 50, 75, 0};
static const int kWeight[6] = {0, 2, 2, 3, 5, 0};
U64 fileBB[8], adjFiles[8], passedMask[2][64];

static void initEval() {
    for (int f = 0; f < 8; f++) fileBB[f] = FILE_A << f;
    for (int f = 0; f < 8; f++) adjFiles[f] = (f > 0 ? fileBB[f - 1] : 0) | (f < 7 ? fileBB[f + 1] : 0);
    for (int sq = 0; sq < 64; sq++) {
        int f = sq % 8, r = sq / 8; U64 files = fileBB[f] | adjFiles[f];
        U64 w = 0, b = 0;
        for (int rr = r + 1; rr < 8; rr++) w |= RANK_1 << (8 * rr);
        for (int rr = r - 1; rr >= 0; rr--) b |= RANK_1 << (8 * rr);
        passedMask[WHITE][sq] = files & w; passedMask[BLACK][sq] = files & b;
    }
}
static inline int npm(const Pos &p, int c) {
    return 320 * popc(p.pc[c * 6 + KNIGHT]) + 330 * popc(p.pc[c * 6 + BISHOP]) + 500 * popc(p.pc[c * 6 + ROOK]) + 900 * popc(p.pc[c * 6 + QUEEN]);
}
static inline int centerDist(int sq) { int f = sq & 7, r = sq >> 3; return max(3 - f, f - 4) + max(3 - r, r - 4); }

// ---------------------------------------------------------------- tunable evaluation
struct S { int mg, eg; };
enum {
    O_MAT = 0, O_PST = 6, O_MOBN = O_PST + 384, O_MOBB = O_MOBN + 9, O_MOBR = O_MOBB + 14, O_MOBQ = O_MOBR + 15,
    O_PASSED = O_MOBQ + 28, O_PKT = O_PASSED + 8, O_PKO = O_PKT + 8, O_PFREE = O_PKO + 8,
    O_ISO = O_PFREE + 8, O_DBL, O_BPAIR, O_ROPEN, O_RSEMI, O_SHIELD, O_SHOPEN, O_TPAWN, O_THANG, O_TMINOR, O_TEMPO,
    O_KATT, O_SAFECHK = O_KATT + 4, O_PHALANX = O_SAFECHK + 4, O_SUPPORTED = O_PHALANX + 8, O_BACKWARD = O_SUPPORTED + 8,
    O_OUTPOST, O_ROOK7 = O_OUTPOST + 2,
    O_SHELT, O_STORM = O_SHELT + 8, O_STORMBLK = O_STORM + 8, O_PSAFE, O_RBEHIND = O_PSAFE + 8, O_BADB, O_LONGDIAG, O_MINBEHIND,
    O_KPROT, O_TBYMINOR, O_TBYROOK = O_TBYMINOR + 4, O_TBYKING = O_TBYROOK + 4, O_PUSHTHREAT, O_SPACE, O_WEAKUNOPP, O_KPDIST,
    O_PAWNLESSFLANK, O_KWEAK, NPARAM
};
static S E[NPARAM];
struct EvalTrace { int16_t coef[NPARAM]; int phase; int constMg, constEg; int scaleNum, scaleDen; int mopup; };
static void initEvalParams() {
    for (int t = 0; t < 6; t++) { E[O_MAT + t] = {mgVal[t], egVal[t]}; for (int i = 0; i < 64; i++) E[O_PST + t * 64 + i] = {0, 0}; }
    for (int m = 0; m < 9; m++) E[O_MOBN + m] = {(m - 4) * 4, (m - 4) * 4};
    for (int m = 0; m < 14; m++) E[O_MOBB + m] = {(m - 6) * 4, (m - 6) * 5};
    for (int m = 0; m < 15; m++) E[O_MOBR + m] = {(m - 6) * 2, (m - 6) * 4};
    for (int m = 0; m < 28; m++) E[O_MOBQ + m] = {(m - 12), (m - 12) * 2};
    for (int r = 0; r < 8; r++) {
        E[O_PASSED + r] = {passedMg[r], passedEg[r]};
        int w = r >= 3 ? r - 2 : 0;
        E[O_PKT + r] = {0, 5 * w}; E[O_PKO + r] = {0, -2 * w}; E[O_PFREE + r] = {0, 4 * w};
    }
    E[O_ISO] = {-8, -10}; E[O_DBL] = {-10, -20}; E[O_BPAIR] = {30, 50}; E[O_ROPEN] = {25, 8}; E[O_RSEMI] = {12, 6};
    E[O_SHIELD] = {10, 0}; E[O_SHOPEN] = {-15, 0}; E[O_TPAWN] = {45, 30}; E[O_THANG] = {18, 12}; E[O_TMINOR] = {25, 15}; E[O_TEMPO] = {12, 12};
}
static void applyTunedParams();
// packed mg/eg score (eg in the high 16 bits), as in Stockfish: one add instead of two
static int PK[NPARAM];
static void syncPacked() { for (int i = 0; i < NPARAM; i++) PK[i] = (int)((unsigned)E[i].eg << 16) + E[i].mg; }

// Terms that depend only on pawns and king squares; cached in a per-thread pawn hash table.
template <class ADD>
static void pawnKingTerms(const Pos &p, ADD &&add, U64 *passers) {
    U64 wp = p.pc[PAWN], bp = p.pc[6 + PAWN];
    U64 pAtt[2] = {((wp << 9) & ~FILE_A) | ((wp << 7) & ~FILE_H), ((bp >> 7) & ~FILE_A) | ((bp >> 9) & ~FILE_H)};
    auto kd = [](int a, int b) { return max(abs((a & 7) - (b & 7)), abs((a >> 3) - (b >> 3))); };
    for (int c = 0; c < 2; c++) {
        int them = c ^ 1; U64 ownP = p.pc[c * 6], oppP = p.pc[them * 6];
        for (U64 b = ownP; b; ) {
            int sq = poplsb(b), idx = c == WHITE ? (sq ^ 56) : sq;
            add(c, O_MAT + PAWN, 1); add(c, O_PST + PAWN * 64 + idx, 1);
            int f = sq & 7, rr = c == WHITE ? (sq >> 3) : 7 - (sq >> 3);
            if (!(passedMask[c][sq] & oppP)) {
                add(c, O_PASSED + rr, 1); passers[c] |= BIT(sq);
                if (rr >= 3) {
                    int stop = sq + (c == WHITE ? 8 : -8);
                    add(c, O_PKT + rr, kd(p.kingSq(them), stop));
                    add(c, O_PKO + rr, kd(p.kingSq(c), stop));
                }
            }
            bool weak = false;
            if (!(adjFiles[f] & ownP)) { add(c, O_ISO, 1); weak = true; }
            U64 rankBB = RANK_1 << (8 * (sq >> 3));
            if (adjFiles[f] & rankBB & ownP) add(c, O_PHALANX + rr, 1);
            if (pAtt[c] & BIT(sq)) add(c, O_SUPPORTED + rr, 1);
            else {
                U64 behind = adjFiles[f] & passedMask[c ^ 1][sq];
                int stop = sq + (c == WHITE ? 8 : -8);
                if (stop >= 0 && stop < 64 && !(behind & ownP) && !(adjFiles[f] & rankBB & ownP) && (pAtt[them] & BIT(stop))) { add(c, O_BACKWARD, 1); weak = true; }
            }
            if (weak && !(passedMask[c][sq] & fileBB[f] & oppP)) add(c, O_WEAKUNOPP, 1);
        }
        for (int f = 0; f < 8; f++) { int n = popc(fileBB[f] & ownP); if (n > 1) add(c, O_DBL, n - 1); }
        int ks = p.kingSq(c), kr = ks >> 3, kf = ks & 7, rrK = c == WHITE ? kr : 7 - kr;
        if (rrK <= 1) {
            U64 front = c == WHITE ? (RANK_1 << (8 * (kr + 1))) | (RANK_1 << (8 * (kr + 2))) : (RANK_1 << (8 * (kr - 1))) | (RANK_1 << (8 * (kr - 2)));
            U64 files = fileBB[kf] | adjFiles[kf];
            add(c, O_SHIELD, min(3, popc(front & files & ownP)));
            int open = 0; for (int f = max(0, kf - 1); f <= min(7, kf + 1); f++) if (!(fileBB[f] & ownP)) open++;
            add(c, O_SHOPEN, open);
        }
        auto rel = [&](int sq) { return c == WHITE ? (sq >> 3) : 7 - (sq >> 3); };
        int kRR = rel(ks), kf2 = max(1, min(6, ks & 7));
        for (int f = kf2 - 1; f <= kf2 + 1; f++) {
            int ourR = 0, thR = 0;
            for (U64 b = ownP & fileBB[f]; b; ) { int q = poplsb(b), r = rel(q); if (r >= kRR && (!ourR || r < ourR)) ourR = r; }
            for (U64 b = oppP & fileBB[f]; b; ) { int q = poplsb(b), r = rel(q); if (r >= kRR && (!thR || r < thR)) thR = r; }
            add(c, O_SHELT + ourR, 1);
            if (ourR && thR == ourR + 1) add(c, O_STORMBLK, 1); else add(c, O_STORM + thR, 1);
        }
        static const U64 FLANK[8] = {0x0F0F0F0F0F0F0F0FULL, 0x0F0F0F0F0F0F0F0FULL, 0x0F0F0F0F0F0F0F0FULL, 0x3C3C3C3C3C3C3C3CULL,
                                     0x3C3C3C3C3C3C3C3CULL, 0xF0F0F0F0F0F0F0F0ULL, 0xF0F0F0F0F0F0F0F0ULL, 0xF0F0F0F0F0F0F0F0ULL};
        if (!((wp | bp) & FLANK[ks & 7])) add(c, O_PAWNLESSFLANK, 1);
        if (ownP) { int md = 8; for (U64 b = ownP; b; ) md = min(md, kd(ks, poplsb(b))); add(c, O_KPDIST, md); }
    }
}
struct PawnEntry { U64 key; int32_t mg, eg; U64 passers[2]; };
static const int PAWN_TT = 1 << 14;
static thread_local PawnEntry pawnTT[PAWN_TT];

template <bool TRACE>
static int evaluateT(const Pos &p, EvalTrace *T) {
    int mg[2] = {0, 0}, eg[2] = {0, 0}, phase = 0, sc[2] = {0, 0};
    auto add = [&](int c, int i, int n) {
        if (TRACE) { mg[c] += E[i].mg * n; eg[c] += E[i].eg * n; T->coef[i] += (int16_t)(c == WHITE ? n : -n); }
        else sc[c] += PK[i] * n;
    };
    U64 wp = p.pc[PAWN], bp = p.pc[6 + PAWN];
    U64 pAtt[2] = {((wp << 9) & ~FILE_A) | ((wp << 7) & ~FILE_H), ((bp >> 7) & ~FILE_A) | ((bp >> 9) & ~FILE_H)};
    U64 kZone[2] = {kingAtt[p.kingSq(WHITE)] | BIT(p.kingSq(WHITE)), kingAtt[p.kingSq(BLACK)] | BIT(p.kingSq(BLACK))};
    int kUnits[2] = {0, 0}, kCnt[2] = {0, 0}, kExtra[2] = {0, 0}; U64 passers[2] = {0, 0};
    const U64 DARK = 0xAA55AA55AA55AA55ULL, CENTER4 = 0x0000001818000000ULL;
    U64 attAll[2] = {pAtt[0] | kingAtt[p.kingSq(WHITE)], pAtt[1] | kingAtt[p.kingSq(BLACK)]}, minorAtt[2] = {0, 0};
    U64 attT[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    auto kd = [](int a, int b) { return max(abs((a & 7) - (b & 7)), abs((a >> 3) - (b >> 3))); };
    if (TRACE) pawnKingTerms(p, add, passers);
    else {
        U64 pk = p.pkey ^ zPiece[KING][p.kingSq(WHITE)] ^ zPiece[6 + KING][p.kingSq(BLACK)];
        PawnEntry &pe = pawnTT[pk & (PAWN_TT - 1)];
        if (pe.key != pk) {
            int pm[2] = {0, 0}, pg[2] = {0, 0}; U64 ps[2] = {0, 0};
            int ps2[2] = {0, 0};
            pawnKingTerms(p, [&](int c, int i, int n) { ps2[c] += PK[i] * n; }, ps);
            for (int c = 0; c < 2; c++) { pm[c] = (int16_t)(uint16_t)(unsigned)ps2[c]; pg[c] = (int16_t)(uint16_t)((unsigned)(ps2[c] + 0x8000) >> 16); }
            pe.key = pk; pe.mg = pm[0] - pm[1]; pe.eg = pg[0] - pg[1]; pe.passers[0] = ps[0]; pe.passers[1] = ps[1];
        }
        mg[0] += pe.mg; eg[0] += pe.eg; passers[0] = pe.passers[0]; passers[1] = pe.passers[1];
    }
    // free stop square for advanced passers depends on all pieces, so it is not cached
    for (int c = 0; c < 2; c++)
        for (U64 b = passers[c]; b; ) {
            int sq = poplsb(b), rr = c == WHITE ? (sq >> 3) : 7 - (sq >> 3);
            if (rr >= 3 && p.bd[sq + (c == WHITE ? 8 : -8)] == EMPTY) add(c, O_PFREE + rr, 1);
        }
    for (int c = 0; c < 2; c++) {
        int them = c ^ 1; U64 own = p.col[c], ownP = p.pc[c * 6], oppP = p.pc[them * 6];
        U64 mobArea = ~own & ~pAtt[them];
        for (int t = KNIGHT; t < 6; t++) {
            U64 b = p.pc[c * 6 + t];
            while (b) {
                int sq = poplsb(b), idx = c == WHITE ? (sq ^ 56) : sq;
                add(c, O_MAT + t, 1); add(c, O_PST + t * 64 + idx, 1); phase += phaseInc[t];
                U64 a = 0;
                switch (t) {
                    case KNIGHT: case BISHOP: {
                        a = t == KNIGHT ? knightAtt[sq] : bishopA(sq, p.all);
                        add(c, (t == KNIGHT ? O_MOBN : O_MOBB) + popc(a & mobArea), 1);
                        int rr = c == WHITE ? (sq >> 3) : 7 - (sq >> 3);
                        if (rr >= 3 && rr <= 5 && (pAtt[c] & BIT(sq)) && !(passedMask[c][sq] & adjFiles[sq & 7] & oppP)) add(c, O_OUTPOST + (t - KNIGHT), 1);
                        int fwd = sq + (c == WHITE ? 8 : -8);
                        if (fwd >= 0 && fwd < 64 && ((wp | bp) & BIT(fwd))) add(c, O_MINBEHIND, 1);
                        add(c, O_KPROT, kd(sq, p.kingSq(c)));
                        if (t == BISHOP) {
                            add(c, O_BADB, popc(ownP & ((BIT(sq) & DARK) ? DARK : ~DARK)));
                            if (popc(bishopA(sq, wp | bp) & CENTER4) >= 2) add(c, O_LONGDIAG, 1);
                        }
                        break;
                    }
                    case ROOK: {
                        a = rookA(sq, p.all); add(c, O_MOBR + popc(a & mobArea), 1);
                        U64 fb = fileBB[sq & 7];
                        if (!(fb & (wp | bp))) add(c, O_ROPEN, 1); else if (!(fb & ownP)) add(c, O_RSEMI, 1);
                        if ((c == WHITE ? (sq >> 3) : 7 - (sq >> 3)) == 6) add(c, O_ROOK7, 1);
                        break;
                    }
                    case QUEEN: { a = bishopA(sq, p.all) | rookA(sq, p.all); add(c, O_MOBQ + popc(a & mobArea), 1); break; }
                    default: break;
                }
                attAll[c] |= a; if (t == KNIGHT || t == BISHOP) minorAtt[c] |= a;
                if (t >= KNIGHT && t <= QUEEN) {
                    attT[c][t - KNIGHT] |= a;
                    U64 z = a & kZone[them];
                    if (z) { kCnt[c]++; kUnits[c] += kWeight[t] * popc(z); add(c, O_KATT + (t - KNIGHT), popc(z)); }
                }
            }
        }
        if (popc(p.pc[c * 6 + BISHOP]) >= 2) add(c, O_BPAIR, 1);
    }
    for (int c = 0; c < 2; c++) {
        int them = c ^ 1;
        U64 theirPieces = p.col[them] & ~p.pc[them * 6 + PAWN] & ~p.pc[them * 6 + KING];
        add(c, O_TPAWN, popc(pAtt[c] & theirPieces));
        add(c, O_THANG, popc(attAll[c] & ~attAll[them] & ~pAtt[c] & theirPieces));
        add(c, O_TMINOR, popc(minorAtt[c] & (p.pc[them * 6 + ROOK] | p.pc[them * 6 + QUEEN])));
    }
    for (int c = 0; c < 2; c++) {
        int them = c ^ 1, ks = p.kingSq(them);
        U64 safe = ~p.col[c] & ~attAll[them];
        U64 nC = knightAtt[ks], bC = bishopA(ks, p.all), rC = rookA(ks, p.all);
        add(c, O_SAFECHK + 0, popc(attT[c][0] & nC & safe));
        add(c, O_SAFECHK + 1, popc(attT[c][1] & bC & safe));
        add(c, O_SAFECHK + 2, popc(attT[c][2] & rC & safe));
        add(c, O_SAFECHK + 3, popc(attT[c][3] & (bC | rC) & safe));
        kExtra[c] += 3 * popc(attT[c][0] & nC & safe) + 2 * popc(attT[c][1] & bC & safe) + 3 * popc(attT[c][2] & rC & safe) + 3 * popc(attT[c][3] & (bC | rC) & safe);
    }
    for (int c = 0; c < 2; c++) {
        int them = c ^ 1; U64 ownP = p.pc[c * 6], oppP = p.pc[them * 6];
        auto rel = [&](int sq) { return c == WHITE ? (sq >> 3) : 7 - (sq >> 3); };
        // passed pawn: safe stop square, rook behind
        for (U64 b = passers[c]; b; ) {
            int sq = poplsb(b), rr = rel(sq);
            if (rr < 3) continue;
            int stop = sq + (c == WHITE ? 8 : -8);
            if (!(attAll[them] & BIT(stop))) add(c, O_PSAFE + rr, 1);
            for (U64 rb = p.pc[c * 6 + ROOK] & fileBB[sq & 7]; rb; ) { int rs = poplsb(rb); if (rel(rs) < rr && (rookA(rs, p.all) & BIT(sq))) add(c, O_RBEHIND, 1); }
        }
        // threats
        for (int vt = KNIGHT; vt <= QUEEN; vt++) {
            U64 v = p.pc[them * 6 + vt];
            add(c, O_TBYMINOR + vt - 1, popc(minorAtt[c] & v));
            add(c, O_TBYROOK + vt - 1, popc(attT[c][2] & v));
        }
        U64 theirNP = p.col[them] & ~oppP & ~p.pc[them * 6 + KING];
        add(c, O_TBYKING, popc(kingAtt[p.kingSq(c)] & theirNP & ~attAll[them]));
        U64 push = c == WHITE ? (ownP << 8) & ~p.all : (ownP >> 8) & ~p.all;
        push |= c == WHITE ? ((push & (RANK_1 << 16)) << 8) & ~p.all : ((push & (RANK_1 << 40)) >> 8) & ~p.all;
        push &= ~pAtt[them] & (attAll[c] | ~attAll[them]);
        U64 pushAtt = c == WHITE ? ((push << 9) & ~FILE_A) | ((push << 7) & ~FILE_H) : ((push >> 7) & ~FILE_A) | ((push >> 9) & ~FILE_H);
        add(c, O_PUSHTHREAT, popc(pushAtt & theirNP));
        // space: safe central squares behind our pawn chain (middlegame)
        U64 area = c == WHITE ? 0x000000003C3C3C00ULL : 0x003C3C3C00000000ULL;
        U64 safe = area & ~ownP & ~pAtt[them];
        U64 behind = ownP; behind |= c == WHITE ? behind >> 8 : behind << 8; behind |= c == WHITE ? behind >> 16 : behind << 16;
        int cnt = popc(safe) + popc(behind & safe & ~attAll[them]);
        add(c, O_SPACE, cnt * popc(p.col[c] & ~ownP) / 4);
        // weak squares around the enemy king
        U64 weakK = kZone[them] & attAll[c] & ~(pAtt[them] | minorAtt[them] | attT[them][2]);
        add(c, O_KWEAK, popc(weakK));
        kExtra[c] += popc(weakK);
    }
    add(p.side, O_TEMPO, 1);
    if (!TRACE) for (int c = 0; c < 2; c++) { mg[c] += (int16_t)(uint16_t)(unsigned)sc[c]; eg[c] += (int16_t)(uint16_t)((unsigned)(sc[c] + 0x8000) >> 16); }
    int ksMg[2] = {0, 0};
    for (int c = 0; c < 2; c++) if (kCnt[c] >= 2) { int u = kUnits[c] + kExtra[c]; if (!p.pc[c * 6 + QUEEN]) u /= 2; ksMg[c ^ 1] -= min(u * u / 4, 500); }
    mg[0] += ksMg[0]; mg[1] += ksMg[1];
    int ph = min(phase, 24);
    int v = ((mg[0] - mg[1]) * ph + (eg[0] - eg[1]) * (24 - ph)) / 24;
    int strong = v > 0 ? WHITE : BLACK, weak = strong ^ 1;
    int ns = npm(p, strong), nw = npm(p, weak);
    int scaleNum = 1, scaleDen = 1, mop = 0;
    if (!p.pc[strong * 6] && ns - nw < 400) { scaleDen = 8; v /= 8; }
    else if (popc(p.pc[BISHOP]) == 1 && popc(p.pc[6 + BISHOP]) == 1 && !(p.pc[KNIGHT] | p.pc[6 + KNIGHT] | p.pc[ROOK] | p.pc[6 + ROOK] | p.pc[QUEEN] | p.pc[6 + QUEEN])
             && (bool)(p.pc[BISHOP] & DARK) != (bool)(p.pc[6 + BISHOP] & DARK)) { scaleDen = 2; v /= 2; }
    else if (!p.pc[weak * 6] && nw == 0 && ns >= 500) {
        int wk = p.kingSq(weak), sk = p.kingSq(strong);
        int dist = abs((wk & 7) - (sk & 7)) + abs((wk >> 3) - (sk >> 3));
        int bonus = 15 * centerDist(wk) + 6 * (14 - dist);
        mop = strong == WHITE ? bonus : -bonus;
        v += mop;
    }
    if (TRACE) { T->phase = ph; T->constMg = ksMg[0] - ksMg[1]; T->constEg = 0; T->scaleNum = scaleNum; T->scaleDen = scaleDen; T->mopup = mop; }
    return p.side == WHITE ? v : -v;
}
static int evaluate(const Pos &p) { return evaluateT<false>(p, nullptr); }
static const short TUNED[572][2] = {
{83,97},{349,282},{356,300},{491,485},{1389,589},{0,0},{0,0},{0,0},
{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{166,-97},{56,-11},
{125,-9},{129,-20},{129,-20},{125,-9},{56,-11},{166,-97},{3,3},{5,24},
{-12,29},{4,29},{4,29},{-12,29},{5,24},{3,3},{-6,6},{-1,6},
{1,5},{-3,2},{-3,2},{1,5},{-1,6},{-6,6},{-15,6},{-7,9},
{2,0},{5,0},{5,0},{2,0},{-7,9},{-15,6},{-10,-3},{7,-2},
{-2,4},{-5,5},{-5,5},{-2,4},{7,-2},{-10,-3},{-20,1},{6,7},
{-3,10},{-22,17},{-22,17},{-3,10},{6,7},{-20,1},{0,0},{0,0},
{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{-168,-15},{2,-8},
{-96,26},{33,7},{33,7},{-96,26},{2,-8},{-168,-15},{-3,-15},{47,-21},
{35,6},{61,17},{61,17},{35,6},{47,-21},{-3,-15},{14,-13},{22,3},
{45,18},{52,19},{52,19},{45,18},{22,3},{14,-13},{25,-7},{20,2},
{38,23},{28,33},{28,33},{38,23},{20,2},{25,-7},{14,-3},{21,4},
{25,16},{26,14},{26,14},{25,16},{21,4},{14,-3},{-9,-28},{9,-15},
{11,-5},{18,8},{18,8},{11,-5},{9,-15},{-9,-28},{0,-49},{5,-19},
{0,-29},{8,-16},{8,-16},{0,-29},{5,-19},{0,-49},{-11,-68},{-15,-30},
{-24,-18},{3,-34},{3,-34},{-24,-18},{-15,-30},{-11,-68},{-55,4},{-66,43},
{-33,38},{-12,29},{-12,29},{-33,38},{-66,43},{-55,4},{-16,15},{-6,0},
{-1,19},{9,25},{9,25},{-1,19},{-6,0},{-16,15},{14,6},{21,7},
{20,9},{27,19},{27,19},{20,9},{21,7},{14,6},{15,-1},{26,9},
{37,10},{35,17},{35,17},{37,10},{26,9},{15,-1},{14,-8},{25,0},
{25,14},{31,12},{31,12},{25,14},{25,0},{14,-8},{11,-12},{27,1},
{17,-8},{19,9},{19,9},{17,-8},{27,1},{11,-12},{17,-6},{20,-32},
{18,-13},{18,-4},{18,-4},{18,-13},{20,-32},{17,-6},{-20,-17},{31,-22},
{1,-16},{-4,-4},{-4,-4},{1,-16},{31,-22},{-20,-17},{11,21},{25,15},
{2,24},{35,11},{35,11},{2,24},{25,15},{11,21},{19,7},{-2,12},
{12,12},{37,0},{37,0},{12,12},{-2,12},{19,7},{1,21},{28,10},
{15,21},{24,14},{24,14},{15,21},{28,10},{1,21},{-4,18},{11,13},
{12,18},{18,7},{18,7},{12,18},{11,13},{-4,18},{-23,12},{-6,6},
{-13,20},{0,5},{0,5},{-13,20},{-6,6},{-23,12},{-24,3},{-6,-10},
{-9,-3},{-12,3},{-12,3},{-9,-3},{-6,-10},{-24,3},{-33,-17},{-8,-19},
{3,-22},{-7,-16},{-7,-16},{3,-22},{-8,-19},{-33,-17},{-8,-20},{-6,-10},
{7,-14},{16,-20},{16,-20},{7,-14},{-6,-10},{-8,-20},{51,-61},{92,-94},
{124,-90},{96,-66},{96,-66},{124,-90},{92,-94},{51,-61},{60,-54},{48,-39},
{77,-28},{88,-30},{88,-30},{77,-28},{48,-39},{60,-54},{76,-87},{90,-57},
{48,1},{65,4},{65,4},{48,1},{90,-57},{76,-87},{79,-82},{72,-49},
{44,10},{39,38},{39,38},{44,10},{72,-49},{79,-82},{62,-75},{66,-68},
{54,-15},{54,5},{54,5},{54,-15},{66,-68},{62,-75},{71,-96},{68,-74},
{65,-44},{54,-33},{54,-33},{65,-44},{68,-74},{71,-96},{60,-153},{73,-138},
{73,-115},{76,-102},{76,-102},{73,-115},{73,-138},{60,-153},{51,-127},{56,-155},
{64,-135},{67,-113},{67,-113},{64,-135},{56,-155},{51,-127},{-248,-68},{3,-42},
{-33,19},{73,14},{73,14},{-33,19},{3,-42},{-248,-68},{-68,-30},{-70,59},
{11,41},{91,19},{91,19},{11,41},{-70,59},{-68,-30},{74,-2},{32,27},
{-23,39},{-78,42},{-78,42},{-23,39},{32,27},{74,-2},{-22,6},{52,12},
{-28,32},{-67,32},{-67,32},{-28,32},{52,12},{-22,6},{2,-8},{12,6},
{-6,7},{-25,13},{-25,13},{-6,7},{12,6},{2,-8},{-1,-4},{25,-6},
{7,0},{-9,-1},{-9,-1},{7,0},{25,-6},{-1,-4},{14,-6},{11,3},
{-9,-1},{-41,0},{-41,0},{-9,-1},{11,3},{14,-6},{-8,1},{23,-8},
{-22,-9},{-13,-24},{-13,-24},{-22,-9},{23,-8},{-8,1},{-23,-64},{-9,-27},
{-2,-15},{4,-4},{8,5},{11,5},{14,6},{18,-1},{26,-7},{-9,-65},
{-5,-18},{4,-17},{8,-7},{10,1},{13,6},{13,9},{14,7},{15,9},
{12,10},{21,0},{9,7},{36,-24},{16,0},{-20,-60},{-12,-22},{-10,-15},
{-7,-7},{-5,1},{-5,7},{-2,12},{0,14},{3,19},{5,20},{6,23},
{10,24},{13,26},{8,25},{73,0},{29,-31},{30,4},{33,-31},{35,-15},
{37,2},{39,14},{43,16},{44,34},{45,45},{43,70},{47,71},{46,78},
{41,87},{39,91},{43,92},{47,85},{47,82},{52,74},{75,47},{91,30},
{110,-1},{285,-148},{245,-111},{490,-271},{452,-252},{489,-323},{681,-382},{190,-257},
{0,0},{-7,14},{-12,19},{-39,14},{-49,21},{33,-18},{203,-9},{0,0},
{0,0},{0,0},{0,0},{-4,13},{-5,20},{1,31},{-12,42},{0,25},
{0,0},{0,0},{0,0},{12,-12},{18,-14},{-1,-8},{-25,-4},{0,-10},
{0,0},{0,0},{0,0},{6,16},{-6,34},{7,44},{55,74},{0,20},
{-4,-4},{-10,-23},{19,55},{32,-11},{12,16},{3,-3},{-10,5},{49,31},
{17,2},{32,13},{15,4},{-1,-5},{1,2},{-1,1},{-1,7},{35,-4},
{5,11},{44,-4},{21,-7},{0,0},{4,0},{4,3},{6,6},{13,29},
{56,67},{326,182},{0,0},{0,0},{0,0},{12,9},{9,8},{13,20},
{54,34},{49,77},{0,0},{-1,2},{14,9},{13,7},{17,9},{-18,10},
{9,-6},{-2,-3},{-8,-2},{-7,-1},{4,4},{55,-13},{0,0},{-3,-2},
{44,16},{-23,12},{-7,8},{-1,0},{4,-4},{-7,-2},{0,0},{5,-10},
{0,0},{0,0},{0,0},{-12,16},{-1,19},{-8,43},{-21,66},{0,0},
{12,12},{-1,-5},{14,13},{4,13},{-2,-3},{12,17},{24,17},{11,-2},
{-2,-1},{10,10},{10,7},{-84,30},{47,16},{47,1},{13,5},{2,-2},
{-6,-14},{3,-16},{90,-52},{6,-2}};
static bool useTuned = true;
static void applyTunedParams() { if (useTuned) for (int k = 0; k < (int)(sizeof(TUNED) / sizeof(TUNED[0])) && k < NPARAM; k++) E[k] = {TUNED[k][0], TUNED[k][1]}; syncPacked(); }

// ---------------------------------------------------------------- SEE
static const int seeVal[6] = {100, 320, 330, 500, 950, 20000};
static bool seeGE(const Pos &p, int m, int threshold) {
    if (mFlag(m) == F_CASTLE || mPromo(m)) return 0 >= threshold;
    int from = mFrom(m), to = mTo(m);
    int victim = mFlag(m) == F_EP ? 100 : (p.bd[to] == EMPTY ? 0 : seeVal[p.bd[to] % 6]);
    int swap = victim - threshold;
    if (swap < 0) return false;
    swap = seeVal[p.bd[from] % 6] - swap;
    if (swap <= 0) return true;
    U64 occ = p.all ^ BIT(from) ^ BIT(to);
    if (mFlag(m) == F_EP) occ ^= BIT(to ^ 8);
    int stm = p.side;
    U64 attackers = p.attackersTo(to, occ);
    U64 bq = p.pc[BISHOP] | p.pc[QUEEN] | p.pc[6 + BISHOP] | p.pc[6 + QUEEN];
    U64 rq = p.pc[ROOK] | p.pc[QUEEN] | p.pc[6 + ROOK] | p.pc[6 + QUEEN];
    int res = 1;
    while (true) {
        stm ^= 1; attackers &= occ;
        U64 my = attackers & p.col[stm];
        if (!my) break;
        res ^= 1;
        U64 bb;
        if ((bb = my & p.pc[stm * 6 + PAWN])) { if ((swap = 100 - swap) < res) break; occ ^= BIT(lsb(bb)); attackers |= bishopA(to, occ) & bq; }
        else if ((bb = my & p.pc[stm * 6 + KNIGHT])) { if ((swap = 320 - swap) < res) break; occ ^= BIT(lsb(bb)); }
        else if ((bb = my & p.pc[stm * 6 + BISHOP])) { if ((swap = 330 - swap) < res) break; occ ^= BIT(lsb(bb)); attackers |= bishopA(to, occ) & bq; }
        else if ((bb = my & p.pc[stm * 6 + ROOK])) { if ((swap = 500 - swap) < res) break; occ ^= BIT(lsb(bb)); attackers |= rookA(to, occ) & rq; }
        else if ((bb = my & p.pc[stm * 6 + QUEEN])) { if ((swap = 950 - swap) < res) break; occ ^= BIT(lsb(bb)); attackers |= (bishopA(to, occ) & bq) | (rookA(to, occ) & rq); }
        else return (attackers & ~p.col[stm]) ? res ^ 1 : res;
    }
    return res;
}

// ---------------------------------------------------------------- TT
const int MATE = 31000, MAXPLY = 128, MATE_IN_MAX = MATE - MAXPLY, INF = 32000, NONE = 32001;
enum { B_NONE, B_UPPER, B_LOWER, B_EXACT };
struct TTE { int32_t move = 0; int16_t score = 0; int8_t depth = 0; uint8_t flag = 0; bool pv = false; };
struct TTSlot { atomic<U64> k{0}, d{0}; };
// buckets of 2 slots; ttSize = number of buckets. move field: bits 0-19 move, bits 24-31 generation
static TTSlot *TT = nullptr; static size_t ttSize = 1; static uint8_t ttGen = 0;
static void ttClear() { for (size_t i = 0; i < 2 * ttSize; i++) { TT[i].k.store(0, memory_order_relaxed); TT[i].d.store(0, memory_order_relaxed); } ttGen = 0; }
static void ttResize(int mb) { delete[] TT; ttSize = max<size_t>(1, (size_t)mb * 1024 * 1024 / (2 * sizeof(TTSlot))); TT = new TTSlot[2 * ttSize]; ttClear(); }
static inline U64 ttPack(const TTE &e) { return (U64)(uint32_t)e.move | ((U64)(uint16_t)e.score << 32) | ((U64)(uint8_t)e.depth << 48) | ((U64)e.flag << 56); }
static inline TTE ttUnpack(U64 d) { TTE e; e.move = (int32_t)(uint32_t)d; e.score = (int16_t)(d >> 32); e.depth = (int8_t)(d >> 48); e.flag = (uint8_t)(d >> 56); return e; }
static inline size_t ttIndex(U64 key) { return (size_t)mulhi64(key, (U64)ttSize) * 2; }
static inline bool ttProbe(U64 key, TTE &e) {
    size_t b = ttIndex(key);
    for (int i = 0; i < 2; i++) {
        U64 k = TT[b + i].k.load(memory_order_relaxed), d = TT[b + i].d.load(memory_order_relaxed);
        if ((k ^ d) == key) { e = ttUnpack(d); e.pv = (e.move >> 20) & 1; e.move &= 0xFFFFF; return true; }
    }
    e = TTE(); return false;
}
static int ttHashfull() {
    int n = 0; for (size_t i = 0; i < 1000 && i < 2 * ttSize; i++) { U64 d = TT[i].d.load(memory_order_relaxed); if (d && ((d >> 24) & 0xFF) == ttGen) n++; }
    return (int)min<size_t>(1000, n * 1000 / min<size_t>(1000, 2 * ttSize));
}
static inline int toTT(int s, int ply) { return s >= MATE_IN_MAX ? s + ply : s <= -MATE_IN_MAX ? s - ply : s; }
static inline int fromTT(int s, int ply) { return s >= MATE_IN_MAX ? s - ply : s <= -MATE_IN_MAX ? s + ply : s; }
static void ttStore(U64 key, int move, int score, int depth, int flag, int ply, bool pv = false) {
    size_t b = ttIndex(key); int pick = -1, worst = 1 << 30;
    for (int i = 0; i < 2; i++) {
        U64 k = TT[b + i].k.load(memory_order_relaxed), d = TT[b + i].d.load(memory_order_relaxed);
        if ((k ^ d) == key) { pick = i; break; }
        TTE o = ttUnpack(d); int age = (uint8_t)(ttGen - (uint8_t)((uint32_t)o.move >> 24));
        int val = (d ? o.depth : -100) - 8 * age;
        if (val < worst) { worst = val; pick = i; }
    }
    TTSlot &s = TT[b + pick]; U64 k = s.k.load(memory_order_relaxed), d = s.d.load(memory_order_relaxed);
    bool same = (k ^ d) == key; TTE old = ttUnpack(d);
    bool oldGen = (uint8_t)((uint32_t)old.move >> 24) != ttGen;
    if (!same || oldGen || depth + 3 >= old.depth || flag == B_EXACT) {
        TTE e; e.move = ((move || !same) ? move : (old.move & 0xFFFFF)) | ((int)pv << 20) | ((int)ttGen << 24);
        e.score = (int16_t)toTT(score, ply); e.depth = (int8_t)max(depth, -1); e.flag = (uint8_t)flag;
        U64 nd = ttPack(e);
        s.k.store(key ^ nd, memory_order_relaxed); s.d.store(nd, memory_order_relaxed);
    }
}


// ---------------------------------------------------------------- NNUE
// Network: (768 inputs x 8 king buckets -> 1024) x 2 perspectives -> 1, SCReLU, QA=255 QB=64.
// File format (bullet "PSN1"): 8 x u32 header {magic, version, inputs, buckets, hidden, QA, QB, scale},
// then int16: FT weights [bucket][feature][hidden], FT bias [hidden], output weights [2*hidden] (side to move first), output bias.
// Feature = colour_rel*384 + piece_type*64 + square, squares and colours seen from each side (black flips ranks).
// King bucket = (king on own ranks 5-8 ? 4 : 0) + king_file / 2.
namespace NN {
const int INPUTS = 768, MAXBUCKETS = 8, MAXHIDDEN = 1024;
int HIDDEN = 1024;
int buckets = 8, QA = 255, QB = 64, SCALE = 400;
bool loaded = false; string loadedName; bool fastDot = false;
int evalNumer = 1, evalDenom = 1;     // final calibration factor applied to network output
alignas(64) static int16_t ftW[MAXBUCKETS * INPUTS * MAXHIDDEN];
alignas(64) static int16_t ftB[MAXHIDDEN];
const int MAXOB = 8;
int outBuckets = 1;                   // version 3 nets: 8 output buckets chosen by piece count
alignas(64) static int16_t outW[MAXOB * 2 * MAXHIDDEN];
static int16_t outB[MAXOB];

bool load(const string &path) {
    ifstream f(path, ios::binary);
    if (!f) return false;
    uint32_t h[8];
    if (!f.read((char *)h, sizeof(h))) return false;
    if (h[0] != 0x314E5350u || h[2] != INPUTS || h[3] < 1 || h[3] > MAXBUCKETS || h[4] < 16 || h[4] > (uint32_t)MAXHIDDEN || h[4] % 16) return false;
    HIDDEN = (int)h[4];
    int nb = (int)h[3];
    size_t nW = (size_t)nb * INPUTS * HIDDEN;
    if (!f.read((char *)ftW, nW * 2)) return false;
    if (!f.read((char *)ftB, HIDDEN * 2)) return false;
    int ob = h[1] == 3 ? 8 : 1;
    if (!f.read((char *)outW, (size_t)ob * 2 * HIDDEN * 2)) return false;
    if (!f.read((char *)outB, ob * 2)) return false;
    outBuckets = ob;
    buckets = nb; QA = (int)h[5]; QB = (int)h[6]; SCALE = (int)h[7];
    // version-1 nets (user-trained 13.x) output ~3.5x centipawns; version-2 nets are trained in centipawns
    if (h[1] == 1) { evalNumer = 2; evalDenom = 7; } else { evalNumer = 1; evalDenom = 1; }
    int mx = 0; for (int i = 0; i < ob * 2 * HIDDEN; i++) mx = max(mx, abs((int)outW[i]));
    fastDot = mx * QA < 32768;     // clamp(x)*w fits in int16 -> SIMD path is exact
    loaded = true; loadedName = path;
    return true;
}
static inline int bucketOf(int ksqRel) { return buckets == 8 ? (ksqRel >= 32 ? 4 : 0) + (ksqRel & 7) / 2 : 0; }
static inline int relSq(int persp, int sq) { return persp == WHITE ? sq : sq ^ 56; }
static inline int featIdx(int persp, int piece, int sq) { return ((piece / 6) ^ persp) * 384 + (piece % 6) * 64 + relSq(persp, sq); }

struct Acc { alignas(64) int16_t v[2][MAXHIDDEN]; int8_t bk[2]; };

static inline void addRow(int16_t *a, const int16_t *w) { for (int i = 0; i < HIDDEN; i++) a[i] += w[i]; }
static inline void subRow(int16_t *a, const int16_t *w) { for (int i = 0; i < HIDDEN; i++) a[i] -= w[i]; }
static inline const int16_t *row(int bk, int f) { return ftW + ((size_t)bk * INPUTS + f) * HIDDEN; }

static void refresh(Acc &a, const Pos &p, int persp) {
    int bk = bucketOf(relSq(persp, p.kingSq(persp)));
    a.bk[persp] = (int8_t)bk;
    memcpy(a.v[persp], ftB, HIDDEN * sizeof(int16_t));
    for (int pc = 0; pc < 12; pc++) { U64 b = p.pc[pc]; while (b) addRow(a.v[persp], row(bk, featIdx(persp, pc, poplsb(b)))); }
}
static void refreshAll(Acc &a, const Pos &p) { refresh(a, p, WHITE); refresh(a, p, BLACK); }
// child = parent + changes recorded in p (the position after make)
static void update(const Acc &parent, Acc &child, const Pos &p) {
    for (int persp = 0; persp < 2; persp++) {
        int bk = bucketOf(relSq(persp, p.kingSq(persp)));
        if (bk != parent.bk[persp]) { refresh(child, p, persp); continue; }
        child.bk[persp] = (int8_t)bk;
        memcpy(child.v[persp], parent.v[persp], HIDDEN * sizeof(int16_t));
        for (int i = 0; i < p.nd; i++) {
            const int16_t *w = row(bk, featIdx(persp, p.dp[i], p.ds[i]));
            if (p.da[i] > 0) addRow(child.v[persp], w); else subRow(child.v[persp], w);
        }
    }
}
static inline int32_t screluDot(const int16_t *a, const int16_t *w) {
#if defined(__AVX2__)
    if (fastDot) {
        const __m256i lo = _mm256_setzero_si256(), hi = _mm256_set1_epi16((short)QA);
        __m256i acc = _mm256_setzero_si256();
        for (int i = 0; i < HIDDEN; i += 16) {
            __m256i v = _mm256_min_epi16(_mm256_max_epi16(_mm256_load_si256((const __m256i *)(a + i)), lo), hi);
            __m256i vw = _mm256_mullo_epi16(v, _mm256_load_si256((const __m256i *)(w + i)));
            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(vw, v));
        }
        __m128i r = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
        r = _mm_add_epi32(r, _mm_shuffle_epi32(r, 0x4E)); r = _mm_add_epi32(r, _mm_shuffle_epi32(r, 0xB1));
        return _mm_cvtsi128_si32(r);
    }
#endif
    int32_t sum = 0;
    for (int i = 0; i < HIDDEN; i++) {
        int32_t x = a[i] < 0 ? 0 : (a[i] > QA ? QA : a[i]);
        sum += (x * w[i]) * x;           // fits int32: 255*53*255 per term
    }
    return sum;
}
static int evaluate(const Acc &a, int stm, int pieces) {
    int ob = outBuckets == 8 ? min(7, max(0, (pieces - 2) / 4)) : 0;
    const int16_t *w = outW + (size_t)ob * 2 * HIDDEN;
    int64_t out = (int64_t)screluDot(a.v[stm], w) + screluDot(a.v[stm ^ 1], w + HIDDEN);
    out = out / QA + outB[ob];
    int v = (int)(out * SCALE / (QA * QB));
    return v * evalNumer / evalDenom;
}
}  // namespace NN

// ---------------------------------------------------------------- search state
static atomic<bool> stopFlag(false);
static atomic<int64_t> startNs(0);
static inline int64_t nowNs() { return chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now().time_since_epoch()).count(); }
static int64_t softLimit, hardLimit, nodeLimit; static atomic<bool> timeLimited(false), infiniteMode(false), pondering(false); static bool ponderTimeLimited = false;
static int maxDepth; static int lastSearchScore = 32001;
static bool quietInfo = false; static int64_t softNodeLimit = 0;
static inline int lmrInit(int d, int m) { return (d && m) ? (int)(0.75 + log((double)d) * log((double)m) / 2.25) : 0; }
static int lmrTab[64][64];
static U64 gameKeys[4096]; static int gameKeyN;
struct Worker {
    int id = 0;
    atomic<U64> nodes{0}; int rootDepth = 0, seldepth = 0, completedDepth = 0, bestMove = 0, bestScore = 0;
    int killers[MAXPLY + 2][2], history[2][64][64], counterMove[12][64];
    int contHist[12][64][12][64], captHist[12][64][6];
    int moveStack[MAXPLY + 2], pieceStack[MAXPLY + 2], evalStack[MAXPLY + 2];
    int pvTab[MAXPLY + 2][MAXPLY + 2], pvLen[MAXPLY + 2];
    U64 keyStack[4096]; int keySP = 0;
    NN::Acc acc[MAXPLY + 4];
    int corrHist[2][16384]; int npCorr[2][2][16384]; int dext[MAXPLY + 2]; U64 rootNodes[4096];
    int pawnHist[512][12][64]; int capStack[MAXPLY + 2]; int cutCnt[MAXPLY + 4]; U64 evCache[1 << 20];
    void clearHist() { memset(history, 0, sizeof(history)); memset(counterMove, 0, sizeof(counterMove)); memset(contHist, 0, sizeof(contHist)); memset(captHist, 0, sizeof(captHist)); memset(corrHist, 0, sizeof(corrHist)); memset(npCorr, 0, sizeof(npCorr)); memset(pawnHist, 0, sizeof(pawnHist)); memset(evCache, 0, sizeof(evCache)); }
};
static vector<unique_ptr<Worker>> workers;
static int nnueBlend = 100;  // 0 = classical, 100 = pure NNUE (only used when a net is loaded and Use NNUE is on)
static inline int classicalEval(Worker &w, const Pos &p) {
    U64 &c = w.evCache[p.key & ((1 << 20) - 1)];
    int v;
    if ((c & ~0xFFFFULL) == (p.key & ~0xFFFFULL) && c) v = (int)(c & 0xFFFF) - 32768;
    else { v = evaluate(p); c = (p.key & ~0xFFFFULL) | (U64)(v + 32768); }
    return v;
}
static inline int nnEval(Worker &w, const Pos &p, int ply) {
    if (!NN::loaded || nnueBlend == 0) return classicalEval(w, p);
#ifdef NNCHECK
    { NN::Acc t; NN::refreshAll(t, p); if (memcmp(t.v[0], w.acc[ply].v[0], NN::HIDDEN * 2) || memcmp(t.v[1], w.acc[ply].v[1], NN::HIDDEN * 2)) { cerr << "ACC MISMATCH ply " << ply << endl; abort(); } }
#endif
    int n = NN::evaluate(w.acc[ply], p.side, popc(p.all));
    n = n * (200 - p.fifty) / 200;                       // fade toward draw as 50-move rule approaches
    int v = nnueBlend == 100 ? n : (classicalEval(w, p) * (100 - nnueBlend) + n * nnueBlend) / 100;
    return max(-MATE_IN_MAX + 1, min(MATE_IN_MAX - 1, v));
}
static const int CORR_GRAIN = 256, CORR_MAX = 256 * 40;
static inline int corrected(Worker &w, const Pos &p, int raw) {
    int c = 2 * w.corrHist[p.side][p.pkey & 16383] + w.npCorr[p.side][0][p.npkey[0] & 16383] + w.npCorr[p.side][1][p.npkey[1] & 16383];
    int v = raw + c / (2 * CORR_GRAIN);
    return max(-MATE_IN_MAX + 1, min(MATE_IN_MAX - 1, v));
}
static inline void updCorr(Worker &w, const Pos &p, int depth, int diff) {
    int wt = min(depth * depth + 2 * depth + 1, 128);
    auto upd = [&](int &e) { e = (e * (256 - wt) + diff * CORR_GRAIN * wt) / 256; e = max(-CORR_MAX, min(CORR_MAX, e)); };
    upd(w.corrHist[p.side][p.pkey & 16383]);
    upd(w.npCorr[p.side][0][p.npkey[0] & 16383]);
    upd(w.npCorr[p.side][1][p.npkey[1] & 16383]);
}
static U64 totalNodes() { U64 n = 0; for (auto &w : workers) n += w->nodes.load(memory_order_relaxed); return n; }
static Pos rootPos;
#if !defined(LANTERN_BROWSER) || defined(LANTERN_BROWSER_PTHREADS)
static mutex ioMutex;
static void say(const string &s) { lock_guard<mutex> lk(ioMutex); cout << s << endl; }
#else
static void say(const string &s) { cout << s << endl; }
#endif
static int64_t elapsed() { return (nowNs() - startNs.load()) / 1000000; }
static inline void checkTime(Worker &w) {
    if (w.id != 0) return;
    if (w.rootDepth <= 1) return;
    if (timeLimited && elapsed() >= hardLimit) stopFlag = true;
    if (nodeLimit && (int64_t)totalNodes() >= nodeLimit) stopFlag = true;
}
static inline bool isRep(Worker &w, const Pos &p) {
    int lim = max(0, w.keySP - 1 - p.fifty);
    for (int i = w.keySP - 3; i >= lim; i -= 2) if (w.keyStack[i] == p.key) return true;
    return false;
}
static inline bool hasNonPawn(const Pos &p, int c) { return p.pc[c * 6 + KNIGHT] | p.pc[c * 6 + BISHOP] | p.pc[c * 6 + ROOK] | p.pc[c * 6 + QUEEN]; }
static inline bool insufficient(const Pos &p) {
    if (p.pc[PAWN] | p.pc[6] | p.pc[ROOK] | p.pc[6 + ROOK] | p.pc[QUEEN] | p.pc[6 + QUEEN]) return false;
    return popc(p.col[WHITE]) <= 2 && popc(p.col[BLACK]) <= 2;
}
static inline bool isCap(const Pos &p, int m) { return p.bd[mTo(m)] != EMPTY || mFlag(m) == F_EP; }
static inline void updHist(int &h, int bonus) { h += bonus - h * abs(bonus) / 16384; }
static inline int capVictim(const Pos &p, int m) { return p.bd[mTo(m)] == EMPTY ? PAWN : p.bd[mTo(m)] % 6; }
static inline int *contEntry(Worker &w, int ply, int back, int piece, int to) {
    if (ply < back || ply - back > MAXPLY || !w.moveStack[ply - back]) return nullptr;
    return &w.contHist[w.pieceStack[ply - back]][mTo(w.moveStack[ply - back])][piece][to];
}
static inline int quietScore(Worker &w, const Pos &p, int m, int ply) {
    int pc = p.bd[mFrom(m)], to = mTo(m), s = w.history[p.side][mFrom(m)][to];
    if (int *c1 = contEntry(w, ply, 1, pc, to)) s += *c1;
    if (int *c2 = contEntry(w, ply, 2, pc, to)) s += *c2;
    if (int *c4 = contEntry(w, ply, 4, pc, to)) s += *c4 / 2;
    s += w.pawnHist[p.pkey & 511][pc][to];
    return s;
}

static inline void updQuietHist(Worker &w, const Pos &p, int ply, int mv, int b) {
    int pc = p.bd[mFrom(mv)], to = mTo(mv);
    updHist(w.history[p.side][mFrom(mv)][to], b);
    if (int *c1 = contEntry(w, ply, 1, pc, to)) updHist(*c1, b);
    if (int *c2 = contEntry(w, ply, 2, pc, to)) updHist(*c2, b);
    if (int *c4 = contEntry(w, ply, 4, pc, to)) updHist(*c4, b);
    updHist(w.pawnHist[p.pkey & 511][pc][to], b);
}
static const int QUIET_PENDING = 4500000;   // quiet move not scored yet (sorts after good captures, before killers)
static inline int scoreQuiet(Worker &w, const Pos &p, int m, int ply, int cm) {
    if (m == w.killers[ply][0]) return 4000000;
    if (m == w.killers[ply][1]) return 3900000;
    if (m == cm) return 3800000;
    return ply < MAXPLY ? quietScore(w, p, m, ply) : w.history[p.side][mFrom(m)][mTo(m)];
}
static inline int counterOf(Worker &w, int ply) { return (ply > 0 && w.moveStack[ply - 1]) ? w.counterMove[w.pieceStack[ply - 1]][mTo(w.moveStack[ply - 1])] : 0; }
// staged ordering: score the quiet moves only when the first one is about to be picked
// threat-aware ordering (as in Stockfish): move pieces out of attacks by cheaper pieces, avoid moving into them, like checks
struct Threats { U64 byPawn, byMinor, byRook, chk[6]; };
static void computeThreats(const Pos &p, Threats &T) {
    int us = p.side, them = us ^ 1;
    U64 ep = p.pc[them * 6 + PAWN];
    T.byPawn = them == WHITE ? (((ep << 9) & ~FILE_A) | ((ep << 7) & ~FILE_H)) : (((ep >> 7) & ~FILE_A) | ((ep >> 9) & ~FILE_H));
    U64 mi = 0, ro = 0;
    for (U64 b = p.pc[them * 6 + KNIGHT]; b; ) mi |= knightAtt[poplsb(b)];
    for (U64 b = p.pc[them * 6 + BISHOP]; b; ) mi |= bishopA(poplsb(b), p.all);
    for (U64 b = p.pc[them * 6 + ROOK]; b; ) ro |= rookA(poplsb(b), p.all);
    T.byMinor = T.byPawn | mi; T.byRook = T.byMinor | ro;
    int ks = p.kingSq(them);
    T.chk[PAWN] = pawnAtt[them][ks]; T.chk[KNIGHT] = knightAtt[ks];
    T.chk[BISHOP] = bishopA(ks, p.all); T.chk[ROOK] = rookA(ks, p.all); T.chk[QUEEN] = T.chk[BISHOP] | T.chk[ROOK]; T.chk[KING] = 0;
}
static inline int threatBonus(const Pos &p, int m, const Threats &T) {
    int pt = p.bd[mFrom(m)] % 6; U64 f = BIT(mFrom(m)), t = BIT(mTo(m));
    int b = 0;
    if (T.chk[pt] & t) b += 8000;
    U64 thr = pt == QUEEN ? T.byRook : pt == ROOK ? T.byMinor : (pt == KNIGHT || pt == BISHOP) ? T.byPawn : 0;
    if (thr) {
        int v = pt == QUEEN ? 12000 : pt == ROOK ? 8000 : 5000;
        if (thr & f) b += (thr & t) ? 0 : v;
        else if (thr & t) b -= v;
    }
    return b;
}
static void scorePendingQuiets(Worker &w, const Pos &p, MoveList &ml, int from, int ply) {
    int cm = counterOf(w, ply);
    Threats T; computeThreats(p, T);
    for (int i = from; i < ml.n; i++) if (ml.sc[i] == QUIET_PENDING) {
        int s = scoreQuiet(w, p, ml.mv[i], ply, cm);
        if (s < 3800000) s += threatBonus(p, ml.mv[i], T);
        ml.sc[i] = s;
    }
}
static void scoreMoves(Worker &w, const Pos &p, MoveList &ml, int ttMove, int ply, bool quietsLater = false) {
    int cm = counterOf(w, ply);
    for (int i = 0; i < ml.n; i++) {
        int m = ml.mv[i], s;
        if (m == ttMove) s = 10000000;
        else if (isCap(p, m) || mPromo(m)) {
            if (mPromo(m) && mPromo(m) != QUEEN) s = -2000000 + mPromo(m);
            else {
                int victim = mFlag(m) == F_EP ? 100 : (p.bd[mTo(m)] == EMPTY ? 0 : seeVal[p.bd[mTo(m)] % 6]);
                if (mPromo(m) == QUEEN) victim += 900;
                s = (seeGE(p, m, 0) ? 5000000 : -1000000) + victim * 16 + w.captHist[p.bd[mFrom(m)]][mTo(m)][capVictim(p, m)] / 16;
            }
        }
        else s = quietsLater ? QUIET_PENDING : scoreQuiet(w, p, m, ply, cm);
        ml.sc[i] = s;
    }
}
static inline int pickNext(MoveList &ml, int i) {
    int b = i;
    for (int j = i + 1; j < ml.n; j++) if (ml.sc[j] > ml.sc[b]) b = j;
    swap(ml.mv[i], ml.mv[b]); swap(ml.sc[i], ml.sc[b]);
    return ml.mv[i];
}

static int qsearch(Worker &w, const Pos &p, int alpha, int beta, int ply) {
    w.nodes.store(w.nodes.load(memory_order_relaxed) + 1, memory_order_relaxed); if ((w.nodes.load(memory_order_relaxed) & 2047) == 0) checkTime(w);
    if (stopFlag) return 0;
    if (ply > w.seldepth) w.seldepth = ply;
    if (ply >= MAXPLY - 1) return nnEval(w, p, ply);
    bool inCheck = p.inCheck();
    TTE te; bool hit = ttProbe(p.key, te); int ttMove = 0;
    if (hit) {
        int s = fromTT(te.score, ply);
        if (te.flag == B_EXACT || (te.flag == B_LOWER && s >= beta) || (te.flag == B_UPPER && s <= alpha)) return s;
        ttMove = te.move;
    }
    int best = -INF, standPat = -INF;
    if (!inCheck) {
        standPat = best = corrected(w, p, nnEval(w, p, ply));
        if (best >= beta) return best;
        if (best > alpha) alpha = best;
    }
    MoveList ml; genMoves(p, ml, !inCheck); scoreMoves(w, p, ml, ttMove, MAXPLY);
    int legal = 0, bestMove = 0;
    for (int i = 0; i < ml.n; i++) {
        int m = pickNext(ml, i);
        if (!inCheck) {
            if (ml.sc[i] < 0 && m != ttMove) break;  // remaining are losing captures / underpromos
            int victim = mFlag(m) == F_EP ? 100 : (p.bd[mTo(m)] == EMPTY ? 0 : seeVal[p.bd[mTo(m)] % 6]);
            if (!mPromo(m) && standPat + victim + 200 <= alpha) continue;
        }
        if (inCheck && best > -MATE_IN_MAX && !isCap(p, m) && !mPromo(m) && legal >= 2) { Pos t = p; if (t.make(m)) legal++; continue; }
        Pos np = p; if (!np.make(m)) continue;
        __builtin_prefetch(&TT[ttIndex(np.key)]);
        legal++;
        if (NN::loaded) NN::update(w.acc[ply], w.acc[ply + 1], np);
        int s = -qsearch(w, np, -beta, -alpha, ply + 1);
        if (stopFlag) return 0;
        if (s > best) { best = s; if (s > alpha) { alpha = s; bestMove = m; if (s >= beta) break; } }
    }
    if (inCheck && legal == 0) return -MATE + ply;
    ttStore(p.key, bestMove, best, 0, best >= beta ? B_LOWER : B_UPPER, ply);
    return best;
}

static int search(Worker &w, const Pos &p, int alpha, int beta, int depth, int ply, bool doNull, bool cutNode, int excluded = 0) {
    w.pvLen[ply] = ply;
    if (depth <= 0) return qsearch(w, p, alpha, beta, ply);
    w.nodes.store(w.nodes.load(memory_order_relaxed) + 1, memory_order_relaxed); if ((w.nodes.load(memory_order_relaxed) & 2047) == 0) checkTime(w);
    if (stopFlag) return 0;
    if (ply > w.seldepth) w.seldepth = ply;
    bool root = ply == 0, pvNode = beta - alpha > 1;
    if (!root) {
        if (p.fifty >= 100 || isRep(w, p) || insufficient(p)) return 0;
        if (ply >= MAXPLY - 1) return nnEval(w, p, ply);
        alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }
    bool inCheck = p.inCheck();
    if (inCheck) depth++;
    TTE te; bool hit = ttProbe(p.key, te); TTE *e = &te;
    int ttMove = hit ? e->move : 0, ttScore = hit ? fromTT(e->score, ply) : 0;
    bool ttPv = excluded ? (hit && e->pv) || pvNode : pvNode || (hit && e->pv);
    if (hit && !excluded && !pvNode && e->depth >= depth) {
        if (e->flag == B_EXACT || (e->flag == B_LOWER && ttScore >= beta) || (e->flag == B_UPPER && ttScore <= alpha)) {
            if (ttScore >= beta && ttMove && !isCap(p, ttMove) && !mPromo(ttMove)) {
                if (w.killers[ply][0] != ttMove) { w.killers[ply][1] = w.killers[ply][0]; w.killers[ply][0] = ttMove; }
                if (ply < MAXPLY) updQuietHist(w, p, ply, ttMove, min(10 * depth * depth, 1200));
            }
            return ttScore;
        }
    }
    int staticEval = NONE, ev = NONE, rawEval = NONE;
    bool improving = false;
    if (ply == 0) w.dext[0] = 0;
    if (!inCheck) {
        rawEval = nnEval(w, p, ply);
        staticEval = ev = corrected(w, p, rawEval);
        if (hit && abs(ttScore) < MATE_IN_MAX &&
            (e->flag == B_EXACT || (e->flag == B_LOWER && ttScore > ev) || (e->flag == B_UPPER && ttScore < ev))) ev = ttScore;
        improving = ply >= 2 && w.evalStack[ply - 2] != NONE ? staticEval > w.evalStack[ply - 2]
                  : ply >= 4 && w.evalStack[ply - 4] != NONE ? staticEval > w.evalStack[ply - 4] : true;
    }
    w.evalStack[ply] = staticEval;
    w.killers[ply + 1][0] = w.killers[ply + 1][1] = 0;
    w.cutCnt[ply + 2] = 0;
    bool ttCapture = ttMove && (isCap(p, ttMove) || mPromo(ttMove));

    if (!pvNode && !inCheck && !excluded) {
        // reverse futility pruning
        if (depth <= 8 && ev - 80 * (depth - improving) >= beta && abs(ev) < MATE_IN_MAX) return (ev + beta) / 2;
        // null move pruning
        if (doNull && depth >= 3 && ev >= beta && staticEval >= beta && hasNonPawn(p, p.side)) {
            int R = 3 + depth / 3 + min((ev - beta) / 200, 3);
            Pos np = p; np.makeNull();
            if (NN::loaded) w.acc[ply + 1] = w.acc[ply];
            w.moveStack[ply] = 0; w.pieceStack[ply] = 0; w.capStack[ply] = 0;
            w.keyStack[w.keySP++] = np.key;
            w.dext[ply + 1] = w.dext[ply];
            int s = -search(w, np, -beta, -beta + 1, depth - 1 - R, ply + 1, false, !cutNode);
            w.keySP--;
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_IN_MAX ? beta : s;
        }
        // razoring
        if (depth <= 3 && ev + 200 + 220 * depth <= alpha) {
            int s = qsearch(w, p, alpha - 1, alpha, ply);
            if (s < alpha) return s;
        }
        // ProbCut: a good capture that beats beta by a margin at reduced depth very likely cuts at full depth
        int pcBeta = beta + 220 - 60 * improving;
        if (depth >= 5 && abs(beta) < MATE_IN_MAX && !(hit && e->depth >= depth - 3 && ttScore < pcBeta)) {
            MoveList pl; genMoves(p, pl, true); scoreMoves(w, p, pl, ttMove, ply);
            for (int i = 0; i < pl.n; i++) {
                int m = pickNext(pl, i);
                if (pl.sc[i] < 0) break;
                if (!seeGE(p, m, pcBeta - staticEval)) continue;
                Pos np = p; if (!np.make(m)) continue;
                if (NN::loaded) NN::update(w.acc[ply], w.acc[ply + 1], np);
                w.moveStack[ply] = m; w.pieceStack[ply] = p.bd[mFrom(m)]; w.capStack[ply] = 1;
                w.keyStack[w.keySP++] = np.key; w.dext[ply + 1] = w.dext[ply];
                int s = -qsearch(w, np, -pcBeta, -pcBeta + 1, ply + 1);
                if (s >= pcBeta) s = -search(w, np, -pcBeta, -pcBeta + 1, depth - 4, ply + 1, true, !cutNode);
                w.keySP--;
                if (stopFlag) return 0;
                if (s >= pcBeta) { ttStore(p.key, m, s, depth - 3, B_LOWER, ply); return s; }
            }
        }
    }
    // internal iterative reduction
    if (depth >= 4 && !ttMove && !excluded) depth -= 1 + (cutNode && depth >= 8);

    MoveList ml; genMoves(p, ml, false);
    // lazy ordering: try the TT move before scoring the rest (same order as full scoring)
    bool scored = true;
    if (ttMove) {
        for (int k = 0; k < ml.n; k++) if (ml.mv[k] == ttMove) { swap(ml.mv[0], ml.mv[k]); ml.sc[0] = 10000000; scored = false; break; }
    }
    if (scored) scoreMoves(w, p, ml, ttMove, ply, true);
    int best = -INF, bestMove = 0, legal = 0, oldAlpha = alpha;
    int quiets[64], nq = 0, caps[32], nc = 0; bool skipQuiets = false;
    int lmpLimit = (3 + depth * depth) / (2 - improving);
    for (int i = 0; i < ml.n; i++) {
        if (!scored && i == 1) { scoreMoves(w, p, ml, ttMove, ply, true); scored = true; }
        int m = (!scored && i == 0) ? ml.mv[0] : pickNext(ml, i);
        if (ml.sc[i] == QUIET_PENDING) { scorePendingQuiets(w, p, ml, i, ply); m = pickNext(ml, i); }
        if (m == excluded) continue;
        bool cap = isCap(p, m), quiet = !cap && !mPromo(m);
        if (quiet && skipQuiets) continue;
        if (!root && best > -MATE_IN_MAX && hasNonPawn(p, p.side)) {
            if (quiet) {
                if (!inCheck && depth <= 8 && legal >= lmpLimit) { skipQuiets = true; continue; }
                if (!inCheck && depth <= 8 && staticEval + 100 + 90 * depth <= alpha) { skipQuiets = true; continue; }
                if (depth <= 8 && !seeGE(p, m, -25 * depth * depth)) continue;
                if (depth <= 4 && !inCheck && quietScore(w, p, m, ply) < -3000 * depth) continue;
            } else {
                if (depth <= 6 && !seeGE(p, m, -90 * depth)) continue;
            }
        }
        // singular extension: is the TT move much better than every alternative?
        int ext = 0;
        if (!root && m == ttMove && !excluded && depth >= 7 && e->depth >= depth - 3 && (e->flag == B_LOWER || e->flag == B_EXACT)
            && abs(ttScore) < MATE_IN_MAX && ply < 2 * w.rootDepth) {
            int sBeta = ttScore - 2 * depth;
            int sv = search(w, p, sBeta - 1, sBeta, (depth - 1) / 2, ply, false, cutNode, m);
            if (stopFlag) return 0;
            if (sv < sBeta) ext = (!pvNode && sv < sBeta - 20 && w.dext[ply] < 6) ? 2 : 1;
            else if (sBeta >= beta) return sBeta;      // multi-cut
            else if (ttScore >= beta) ext = -1;
        }
        Pos np = p; if (!np.make(m)) continue;
        __builtin_prefetch(&TT[ttIndex(np.key)]);
        legal++;
        if (NN::loaded) NN::update(w.acc[ply], w.acc[ply + 1], np);
        w.moveStack[ply] = m; w.pieceStack[ply] = p.bd[mFrom(m)]; w.capStack[ply] = !quiet;
        w.keyStack[w.keySP++] = np.key;
        w.dext[ply + 1] = w.dext[ply] + (ext == 2);
        U64 nodesBefore = root ? w.nodes.load(memory_order_relaxed) : 0;
        bool givesCheck = np.inCheck();
        int newDepth = depth - 1 + ext, s;
        if (legal == 1) s = -search(w, np, -beta, -alpha, newDepth, ply + 1, true, pvNode ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && legal > 1 + (int)pvNode) {
                if (quiet) {
                    R = lmrTab[min(depth, 63)][min(legal, 63)];
                    if (pvNode) R--;
                    if (!improving) R++;
                    if (givesCheck) R--;
                    if (m == w.killers[ply][0] || m == w.killers[ply][1]) R--;
                    if (cutNode) R++;
                    if (ttPv) R--;
                    if (ttCapture) R++;
                    if (w.cutCnt[ply + 1] > 3) R++;
                    R -= quietScore(w, p, m, ply) / 11000;
                } else if (ml.sc[i] < 0) R = 1;  // losing captures
                R = max(0, min(R, newDepth - 1));
            }
            int rd = newDepth - R;
            s = -search(w, np, -alpha - 1, -alpha, rd, ply + 1, true, true);
            if (s > alpha && R > 0) {
                newDepth += (s > best + 60 + 2 * newDepth) - (s < best + 10);
                if (newDepth > rd) s = -search(w, np, -alpha - 1, -alpha, newDepth, ply + 1, true, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(w, np, -beta, -alpha, newDepth, ply + 1, true, false);
        }
        w.keySP--;
        if (root) w.rootNodes[m & 4095] += w.nodes.load(memory_order_relaxed) - nodesBefore;
        if (stopFlag) return 0;
        if (s > best) {
            best = s;
            if (s > alpha) {
                alpha = s; bestMove = m;
                w.pvTab[ply][ply] = m;
                for (int j = ply + 1; j < w.pvLen[ply + 1]; j++) w.pvTab[ply][j] = w.pvTab[ply + 1][j];
                w.pvLen[ply] = max(w.pvLen[ply + 1], ply + 1);
                if (s >= beta) {
                    w.cutCnt[ply]++;
                    int bonus = min(16 * depth * depth, 1600);
                    auto updQuiet = [&](int mv, int b) { updQuietHist(w, p, ply, mv, b); };
                    if (quiet) {
                        if (w.killers[ply][0] != m) { w.killers[ply][1] = w.killers[ply][0]; w.killers[ply][0] = m; }
                        updQuiet(m, bonus);
                        for (int q = 0; q < nq; q++) updQuiet(quiets[q], -bonus);
                        if (ply > 0 && w.moveStack[ply - 1]) w.counterMove[w.pieceStack[ply - 1]][mTo(w.moveStack[ply - 1])] = m;
                    } else updHist(w.captHist[p.bd[mFrom(m)]][mTo(m)][capVictim(p, m)], bonus);
                    for (int q = 0; q < nc; q++) updHist(w.captHist[p.bd[mFrom(caps[q])]][mTo(caps[q])][capVictim(p, caps[q])], -bonus);
                    break;
                }
            }
        }
        if (quiet && nq < 64) quiets[nq++] = m;
        else if (!quiet && nc < 32) caps[nc++] = m;
    }
    if (legal == 0) return excluded ? alpha : (inCheck ? -MATE + ply : 0);
    int flag = best >= beta ? B_LOWER : (alpha > oldAlpha ? B_EXACT : B_UPPER);
    if (!excluded) ttStore(p.key, bestMove, best, depth, flag, ply, ttPv);
    // fail low: the opponent's previous quiet move was good, reward it
    if (flag == B_UPPER && !root && ply >= 1 && w.moveStack[ply - 1] && !w.capStack[ply - 1]) {
        int pm = w.moveStack[ply - 1], pc = w.pieceStack[ply - 1], b = min(8 * depth * depth, 800);
        updHist(w.history[p.side ^ 1][mFrom(pm)][mTo(pm)], b);
        if (int *c1 = contEntry(w, ply - 1, 1, pc, mTo(pm))) updHist(*c1, b);
    }
    if (!excluded && !inCheck && (!bestMove || (!isCap(p, bestMove) && !mPromo(bestMove))) && abs(best) < MATE_IN_MAX
        && !(flag == B_LOWER && best <= staticEval) && !(flag == B_UPPER && best >= staticEval))
        updCorr(w, p, depth, best - rawEval);
    return best;
}

static string scoreStr(int s) {
    if (s >= MATE_IN_MAX) return "mate " + to_string((MATE - s + 1) / 2);
    if (s <= -MATE_IN_MAX) return "mate -" + to_string((MATE + s) / 2);
    return "cp " + to_string(s);
}

static void prepWorker(Worker &w) {
    w.nodes.store(0, memory_order_relaxed); w.rootDepth = 0; w.completedDepth = 0; w.bestMove = 0; w.bestScore = 0;
    memset(w.killers, 0, sizeof(w.killers));
    for (auto &a : w.history) for (auto &b : a) for (auto &c : b) c /= 2;
    memcpy(w.keyStack, gameKeys, sizeof(U64) * gameKeyN); w.keySP = gameKeyN;
    memset(w.rootNodes, 0, sizeof(w.rootNodes));
    if (NN::loaded) NN::refreshAll(w.acc[0], rootPos);
}
// iterative deepening for one thread; thread 0 reports and manages time
static void iterate(Worker &w, int nLegal) {
    bool main = w.id == 0;
    int prevScore = 0, prevBest = 0, stable = 0;
    for (int d = 1 + (main ? 0 : (w.id & 1)); d <= maxDepth; d++) {
        w.rootDepth = d; w.seldepth = 0;
        int delta = 18, alpha = -INF, beta = INF, score = 0;
        if (d >= 5) { alpha = max(prevScore - delta, -INF); beta = min(prevScore + delta, INF); }
        while (true) {
            score = search(w, rootPos, alpha, beta, d, 0, false, false);
            if (stopFlag) break;
            if (score <= alpha) { beta = (alpha + beta) / 2; alpha = max(score - delta, -INF); }
            else if (score >= beta) { beta = min(score + delta, INF); if (w.pvLen[0] > 0) w.bestMove = w.pvTab[0][0]; }
            else break;
            delta += delta / 2;
        }
        if (stopFlag && d > 1) break;
        if (w.pvLen[0] > 0) w.bestMove = w.pvTab[0][0];
        w.bestScore = prevScore = score; w.completedDepth = d;
        if (!main) continue;
        if (softNodeLimit && (int64_t)totalNodes() >= softNodeLimit) break;
        if (quietInfo) continue;
        int64_t el = elapsed(); U64 n = totalNodes();
        string pv;
        for (int j = 0; j < w.pvLen[0]; j++) pv += moveStr(w.pvTab[0][j]) + " ";
        say("info depth " + to_string(d) + " seldepth " + to_string(w.seldepth) + " score " + scoreStr(score) +
            " nodes " + to_string(n) + " nps " + to_string(n * 1000 / max<int64_t>(el, 1)) + " time " + to_string(el) + " hashfull " + to_string(ttHashfull()) + " pv " + pv);
        if (w.bestMove == prevBest) stable++; else stable = 0;
        prevBest = w.bestMove;
        if (timeLimited) {
            double factor = stable >= 6 ? 0.55 : stable >= 3 ? 0.8 : stable == 0 ? 1.4 : 1.0;
            U64 tot = max<U64>(1, w.nodes.load(memory_order_relaxed));
            double frac = (double)w.rootNodes[w.bestMove & 4095] / tot;
            if (d >= 6) factor *= max(0.6, min(1.7, (1.55 - frac) * 1.35));
            if (lastSearchScore != 32001 && abs(score) < MATE_IN_MAX) factor *= max(0.75, min(1.5, 1.0 + (lastSearchScore - score) * 0.006));
            if (nLegal == 1) factor = 0.05;
            if (el > softLimit * factor * 0.6) break;
        }
        if (abs(score) >= MATE_IN_MAX && d > 10 && timeLimited && el > softLimit * 0.2) break;
    }
}

static void think() {
    MoveList ml; genMoves(rootPos, ml, false);
    int firstLegal = 0, nLegal = 0;
    for (int i = 0; i < ml.n; i++) { Pos np = rootPos; if (np.make(ml.mv[i])) { if (!firstLegal) firstLegal = ml.mv[i]; nLegal++; } }
    if (nLegal == 0) { say("info depth 0 score " + string(rootPos.inCheck() ? "mate 0" : "cp 0")); say("bestmove 0000"); return; }
    ttGen++;
    for (auto &w : workers) prepWorker(*w);
#if !defined(LANTERN_BROWSER) || defined(LANTERN_BROWSER_PTHREADS)
    vector<thread> helpers;
    for (size_t i = 1; i < workers.size(); i++) helpers.emplace_back(iterate, ref(*workers[i]), nLegal);
#endif
    iterate(*workers[0], nLegal);
#if !defined(LANTERN_BROWSER) || defined(LANTERN_BROWSER_PTHREADS)
    while ((infiniteMode || pondering) && !stopFlag) this_thread::sleep_for(chrono::milliseconds(1));
#endif
    stopFlag = true;
#if !defined(LANTERN_BROWSER) || defined(LANTERN_BROWSER_PTHREADS)
    for (auto &t : helpers) t.join();
#endif
    int bestMove = workers[0]->bestMove;
    lastSearchScore = abs(workers[0]->bestScore) < MATE_IN_MAX ? workers[0]->bestScore : 32001;
    if (!bestMove) bestMove = firstLegal;
    say("bestmove " + moveStr(bestMove));
}

// ---------------------------------------------------------------- UCI
static const string STARTFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
#if !defined(LANTERN_BROWSER) || defined(LANTERN_BROWSER_PTHREADS)
static thread searchThread;
static void waitSearch() { if (searchThread.joinable()) searchThread.join(); }
#else
static void waitSearch() {} // synchronous searches run inside an isolated Web Worker
#endif
static void stopSearch() { stopFlag = true; infiniteMode = false; pondering = false; waitSearch(); }

static int parseMove(const Pos &p, const string &s) {
    MoveList ml; genMoves(p, ml, false);
    for (int i = 0; i < ml.n; i++) if (moveStr(ml.mv[i]) == s) { Pos np = p; if (np.make(ml.mv[i])) return ml.mv[i]; }
    return 0;
}
static void setPosition(istringstream &ss) {
    string tok, fen; ss >> tok;
    if (tok == "startpos") { fen = STARTFEN; ss >> tok; }
    else if (tok == "fen") { while (ss >> tok && tok != "moves") fen += tok + " "; }
    rootPos.setFen(fen);
    gameKeyN = 0; gameKeys[gameKeyN++] = rootPos.key;
    if (tok == "moves") while (ss >> tok) {
        int m = parseMove(rootPos, tok); if (!m) break;
        rootPos.make(m);
        if (rootPos.fifty == 0) gameKeyN = 0;   // positions before an irreversible move can never repeat
        if (gameKeyN < 3800) gameKeys[gameKeyN++] = rootPos.key;
    }
}
static void go(istringstream &ss) {
    bool ponderGo = false;
    string tok; int64_t wtime = -1, btime = -1, winc = 0, binc = 0, mtg = 0, movetime = -1;
    maxDepth = MAXPLY - 4; nodeLimit = 0; infiniteMode = false;
    while (ss >> tok) {
        if (tok == "wtime") ss >> wtime; else if (tok == "btime") ss >> btime;
        else if (tok == "winc") ss >> winc; else if (tok == "binc") ss >> binc;
        else if (tok == "movestogo") ss >> mtg; else if (tok == "movetime") ss >> movetime;
        else if (tok == "depth") ss >> maxDepth; else if (tok == "nodes") ss >> nodeLimit;
        else if (tok == "infinite") infiniteMode = true;
        else if (tok == "ponder") ponderGo = true;
    }
    int64_t t = rootPos.side == WHITE ? wtime : btime, inc = rootPos.side == WHITE ? winc : binc;
    timeLimited = false;
    const int64_t overhead = 30;
    if (movetime >= 0) { timeLimited = true; softLimit = hardLimit = max<int64_t>(1, movetime - overhead); softLimit *= 2; }
    else if (t >= 0) {
        timeLimited = true;
        int64_t left = max<int64_t>(1, t - overhead);
        int64_t div = mtg > 0 ? min<int64_t>(mtg + 1, 40) : 20;
        softLimit = left / div + inc * 3 / 4;
        softLimit = min(softLimit, left * 2 / 3);
        hardLimit = min(left * 3 / 5, softLimit * 4);
        hardLimit = max<int64_t>(hardLimit, 1); softLimit = max<int64_t>(softLimit, 1);
    }
    if (infiniteMode) timeLimited = false;
    if (ponderGo) { ponderTimeLimited = timeLimited; timeLimited = false; pondering = true; } else pondering = false;
    stopFlag = false;
    startNs = nowNs();
#if !defined(LANTERN_BROWSER) || defined(LANTERN_BROWSER_PTHREADS)
    searchThread = thread(think);
#else
    think();
#endif
}

static void setThreads(int n) {
    workers.clear();
    for (int i = 0; i < n; i++) { workers.emplace_back(new Worker()); workers.back()->id = i; workers.back()->clearHist(); }
}
static const char *benchFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
    "r2q1rk1/ppp2ppp/2n1bn2/2b1p3/3pP3/3P1NPP/PPP1NPB1/R1BQ1RK1 b - - 0 9",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
    "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
    "8/8/8/8/5kp1/P7/8/1K1N4 w - - 0 1",
    "8/5pk1/6p1/1p2P2p/1P3P2/6PP/8/6K1 w - - 0 1",
    "2r3k1/pp1q1ppp/4p3/3pP3/3P4/P1rB4/1P3PPP/2RQ2K1 w - - 0 21",
    "6k1/5p2/6p1/8/7p/8/6PP/6K1 b - - 0 0",
};
static void bench(int depth) {
    U64 total = 0; auto t0 = chrono::steady_clock::now();
    for (auto f : benchFens) {
        ttClear(); for (auto &w : workers) w->clearHist();
        rootPos.setFen(f); gameKeyN = 0; gameKeys[gameKeyN++] = rootPos.key;
        maxDepth = depth; timeLimited = false; infiniteMode = false; nodeLimit = 0; stopFlag = false;
        startNs = nowNs();
        think(); total += totalNodes();
    }
    int64_t ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();
    say("Bench: " + to_string(total) + " nodes " + to_string(total * 1000 / max<int64_t>(ms, 1)) + " nps " + to_string(ms) + " ms");
}

// ---------------------------------------------------------------- self-play data generation
#pragma pack(push, 1)
struct PackedPos { U64 occ; uint8_t pcs[16]; int16_t score; int8_t result; uint8_t stm; uint8_t pad[4]; };
#pragma pack(pop)
static PackedPos packPos(const Pos &p, int scoreWhite) {
    PackedPos r; memset(&r, 0, sizeof(r));
    r.occ = p.all; int i = 0; U64 b = p.all;
    while (b) { int sq = poplsb(b); int pc = p.bd[sq]; r.pcs[i / 2] |= (uint8_t)(pc << ((i & 1) * 4)); i++; }
    r.score = (int16_t)max(-32000, min(32000, scoreWhite)); r.stm = (uint8_t)p.side; return r;
}
static int dgRandomPlies = 8;
static void datagen(int games, int softNodes, const string &outFile, U64 seed) {
    FILE *out = fopen(outFile.c_str(), "ab");
    if (!out) { say("cannot open " + outFile); return; }
    auto rn = [&]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    quietInfo = true;
    U64 totalPos = 0; auto t0 = chrono::steady_clock::now();
    vector<PackedPos> buf;
    for (int g = 0; g < games; g++) {
        ttClear(); for (auto &w : workers) w->clearHist();
        rootPos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); gameKeyN = 0; gameKeys[gameKeyN++] = rootPos.key;
        int randomPlies = dgRandomPlies + (int)(rn() % 2);
        bool bad = false;
        for (int i = 0; i < randomPlies; i++) {
            MoveList ml; genMoves(rootPos, ml, false); vector<int> leg;
            for (int k = 0; k < ml.n; k++) { Pos np = rootPos; if (np.make(ml.mv[k])) leg.push_back(ml.mv[k]); }
            if (leg.empty()) { bad = true; break; }
            rootPos.make(leg[rn() % leg.size()]); gameKeys[gameKeyN++] = rootPos.key;
        }
        if (bad) continue;
        buf.clear();
        int result = -1, winCount = 0, drawCount = 0;
        for (int ply = 0; ply < 400 && result < 0; ply++) {
            MoveList ml; genMoves(rootPos, ml, false); int nLegal = 0;
            for (int k = 0; k < ml.n; k++) { Pos np = rootPos; if (np.make(ml.mv[k])) nLegal++; }
            if (nLegal == 0) { result = rootPos.inCheck() ? (rootPos.side == WHITE ? 0 : 2) : 1; break; }
            int reps = 0; for (int k = 0; k < gameKeyN - 1; k++) if (gameKeys[k] == rootPos.key) reps++;
            if (rootPos.fifty >= 100 || reps >= 2 || insufficient(rootPos)) { result = 1; break; }
            maxDepth = MAXPLY - 4; timeLimited = false; infiniteMode = false; nodeLimit = softNodes * 10; softNodeLimit = softNodes; stopFlag = false;
            startNs = nowNs();
            for (auto &w : workers) prepWorker(*w);
            iterate(*workers[0], nLegal);
            int m = workers[0]->bestMove, sc = workers[0]->bestScore;
            if (!m) { result = 1; break; }
            if (ply == 0 && abs(sc) > 400) { bad = true; break; }      // unbalanced opening
            int scW = rootPos.side == WHITE ? sc : -sc;
            if (!rootPos.inCheck() && !isCap(rootPos, m) && !mPromo(m) && abs(sc) < 2000) buf.push_back(packPos(rootPos, scW));
            if (abs(sc) >= 2500) { if (++winCount >= 4) { result = scW > 0 ? 2 : 0; break; } } else winCount = 0;
            if (ply >= 80 && abs(sc) <= 10) { if (++drawCount >= 12) { result = 1; break; } } else drawCount = 0;
            rootPos.make(m);
            if (rootPos.fifty == 0) gameKeyN = 0;
            gameKeys[gameKeyN++] = rootPos.key;
        }
        if (bad) continue;
        if (result < 0) result = 1;
        for (auto &r : buf) r.result = (int8_t)result;
        fwrite(buf.data(), sizeof(PackedPos), buf.size(), out); fflush(out);
        totalPos += buf.size();
        if ((g + 1) % 50 == 0) {
            double sec = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
            cerr << "games " << g + 1 << " positions " << totalPos << " (" << (int)(totalPos / max(sec, 1.0)) << "/s)" << endl;
        }
    }
    fclose(out); quietInfo = false; softNodeLimit = 0; nodeLimit = 0;
}


// ---------------------------------------------------------------- Texel tuner for the classical eval
struct TEntry { uint16_t idx; int16_t coef; };
struct TPos { uint32_t start; uint16_t n; uint8_t phase, scaleDen; int16_t constMg, mop; float target; };
static bool texelSymPst = true;
#ifndef LANTERN_BROWSER
static void texelTune(int iters, double lambda, const string &outFile, const vector<string> &files, size_t maxPos) {
    vector<PackedPos> raw;
    for (auto &fn : files) {
        FILE *f = fopen(fn.c_str(), "rb"); if (!f) { say("cannot open " + fn); return; }
        PackedPos r; while (fread(&r, sizeof(r), 1, f) == 1) raw.push_back(r);
        fclose(f);
    }
    mt19937_64 rng(7); shuffle(raw.begin(), raw.end(), rng);
    if (raw.size() > maxPos) raw.resize(maxPos);
    vector<TEntry> ent; vector<TPos> tp; tp.reserve(raw.size()); ent.reserve(raw.size() * 64);
    EvalTrace T;
    for (auto &r : raw) {
        Pos p; p.clear(); U64 b = r.occ; int i = 0;
        while (b) { int sq = poplsb(b); int pc = (r.pcs[i / 2] >> ((i & 1) * 4)) & 15; p.add(pc, sq); i++; }
        p.side = r.stm; p.castle = 0; p.ep = -1;
        if (!p.pc[KING] || !p.pc[6 + KING]) continue;
        memset(&T, 0, sizeof(T));
        evaluateT<true>(p, &T);
        TPos t; t.start = (uint32_t)ent.size(); t.n = 0;
        for (int k = 0; k < NPARAM; k++) if (T.coef[k]) { ent.push_back({(uint16_t)k, T.coef[k]}); t.n++; }
        t.phase = (uint8_t)T.phase; t.scaleDen = (uint8_t)T.scaleDen; t.constMg = (int16_t)T.constMg; t.mop = (int16_t)T.mopup;
        double wdl = r.result / 2.0, sig = 1.0 / (1.0 + exp(-r.score / 400.0));
        t.target = (float)(lambda * wdl + (1 - lambda) * sig);
        tp.push_back(t);
    }
    say("texel: " + to_string(tp.size()) + " positions, " + to_string(ent.size()) + " trace entries");
    vector<double> pm(NPARAM), pe(NPARAM);
    for (int k = 0; k < NPARAM; k++) { pm[k] = E[k].mg; pe[k] = E[k].eg; }
    auto evalPos = [&](const TPos &t) {
        double mg = t.constMg, eg = 0;
        for (uint32_t j = t.start; j < t.start + t.n; j++) { mg += pm[ent[j].idx] * ent[j].coef; eg += pe[ent[j].idx] * ent[j].coef; }
        return (mg * t.phase + eg * (24 - t.phase)) / 24.0 / t.scaleDen + t.mop;
    };
    int NT = 2;
    auto totalLoss = [&](double K) {
        vector<double> part(NT, 0); vector<thread> th;
        for (int k = 0; k < NT; k++) th.emplace_back([&, k]() { for (size_t i = k; i < tp.size(); i += NT) { double pr = 1 / (1 + exp(-K * evalPos(tp[i]) / 400)); part[k] += (pr - tp[i].target) * (pr - tp[i].target); } });
        for (auto &x : th) x.join();
        return (part[0] + part[1]) / tp.size();
    };
    double bestK = 1, bestL = 1e9;
    for (double K = 0.5; K <= 2.0001; K += 0.05) { double l = totalLoss(K); if (l < bestL) { bestL = l; bestK = K; } }
    say("texel: K = " + to_string(bestK) + " start loss " + to_string(bestL));
    vector<double> mM(NPARAM, 0), vM(NPARAM, 0), mE(NPARAM, 0), vE(NPARAM, 0);
    double lr = 1.0;
    for (int it = 1; it <= iters; it++) {
        vector<vector<double>> gM(NT, vector<double>(NPARAM, 0)), gE(NT, vector<double>(NPARAM, 0));
        vector<double> part(NT, 0); vector<thread> th;
        for (int k = 0; k < NT; k++) th.emplace_back([&, k]() {
            for (size_t i = k; i < tp.size(); i += NT) {
                const TPos &t = tp[i];
                double e = evalPos(t), pr = 1 / (1 + exp(-bestK * e / 400)), err = pr - t.target;
                part[k] += err * err;
                double g = 2 * err * pr * (1 - pr) * bestK / 400 / t.scaleDen / 24.0;
                double gm = g * t.phase, ge = g * (24 - t.phase);
                for (uint32_t j = t.start; j < t.start + t.n; j++) { gM[k][ent[j].idx] += gm * ent[j].coef; gE[k][ent[j].idx] += ge * ent[j].coef; }
            }
        });
        for (auto &x : th) x.join();
        double N = tp.size();
        vector<double> GM(NPARAM), GE(NPARAM);
        for (int p2 = 0; p2 < NPARAM; p2++) { GM[p2] = (gM[0][p2] + gM[1][p2]) / N; GE[p2] = (gE[0][p2] + gE[1][p2]) / N; }
        if (texelSymPst)   // left-right symmetric piece-square tables: a square and its mirror share one value
            for (int t = 0; t < 6; t++) for (int i = 0; i < 64; i++) if ((i & 7) < 4) {
                int x = O_PST + t * 64 + i, y = O_PST + t * 64 + (i ^ 7);
                double m = (GM[x] + GM[y]) / 2, e = (GE[x] + GE[y]) / 2; GM[x] = GM[y] = m; GE[x] = GE[y] = e;
            }
        for (int p2 = 0; p2 < NPARAM; p2++) {
            if (p2 == O_MAT + KING) continue;
            double a = GM[p2], b2 = GE[p2];
            mM[p2] = 0.9 * mM[p2] + 0.1 * a; vM[p2] = 0.999 * vM[p2] + 0.001 * a * a;
            mE[p2] = 0.9 * mE[p2] + 0.1 * b2; vE[p2] = 0.999 * vE[p2] + 0.001 * b2 * b2;
            double c1 = 1 - pow(0.9, it), c2 = 1 - pow(0.999, it);
            pm[p2] -= lr * (mM[p2] / c1) / (sqrt(vM[p2] / c2) + 1e-12);
            pe[p2] -= lr * (mE[p2] / c1) / (sqrt(vE[p2] / c2) + 1e-12);
        }
        if (it % 50 == 0 || it == iters) say("texel: iter " + to_string(it) + " loss " + to_string((part[0] + part[1]) / N));
    }
    FILE *o = fopen(outFile.c_str(), "w");
    fprintf(o, "static const short TUNED[%d][2] = {", NPARAM);
    for (int k = 0; k < NPARAM; k++) fprintf(o, "%s{%d,%d}", k ? (k % 8 ? "," : ",\n") : "\n", (int)lround(pm[k]), (int)lround(pe[k]));
    fprintf(o, "};\n"); fclose(o);
    say("texel: wrote " + outFile);
}

#endif // no offline Texel training needed in WebAssembly
static string exeDir;
static void tryLoadNet(const string &name, bool verbose) {
    vector<string> cands = {name};
    if (!exeDir.empty() && name.find('/') == string::npos && name.find('\\') == string::npos) cands.push_back(exeDir + name);
    for (auto &c : cands) if (NN::load(c)) { if (verbose) say("info string NNUE loaded: " + c); return; }
    NN::loaded = false;
    if (verbose) say("info string NNUE file '" + name + "' not found or invalid - using classical evaluation");
}
#ifndef LANTERN_BROWSER
int main(int argc, char **argv) {
    { string a0 = argc > 0 ? argv[0] : ""; size_t k = a0.find_last_of("/\\"); if (k != string::npos) exeDir = a0.substr(0, k + 1); }
    ios::sync_with_stdio(false);
    initAttacks(); initZobrist(); initEval(); initEvalParams(); applyTunedParams();
    for (int d = 0; d < 64; d++) for (int m = 0; m < 64; m++) lmrTab[d][m] = lmrInit(d, m);
    ttResize(64);
    setThreads(1);
    rootPos.setFen(STARTFEN); gameKeyN = 0; gameKeys[gameKeyN++] = rootPos.key;
    if (argc > 1 && string(argv[1]) == "bench") { bench(argc > 2 ? atoi(argv[2]) : 12); return 0; }
    string line;
    while (getline(cin, line)) {
        istringstream ss(line); string cmd; ss >> cmd;
        if (cmd == "uci") {
            say("id name Lantern 16.4\nid author S Gurucharan team\noption name Hash type spin default 64 min 1 max 8192\noption name Threads type spin default 1 min 1 max 256\noption name Clear Hash type button\noption name Ponder type check default false\noption name EvalFile type string default <empty>\noption name Use NNUE type check default false\noption name NNUE Blend type spin default 100 min 0 max 100\nuciok");
            say(NN::loaded ? "info string NNUE loaded: " + NN::loadedName : "info string NNUE not loaded - using classical evaluation");
        } else if (cmd == "isready") say("readyok");
        else if (cmd == "ucinewgame") { stopSearch(); ttClear(); lastSearchScore = 32001; for (auto &w : workers) w->clearHist(); }
        else if (cmd == "setoption") {
            string t, name, value; ss >> t;
            while (ss >> t && t != "value") name += (name.empty() ? "" : " ") + t;
            ss >> value;
            stopSearch();
            if (name == "Hash" && !value.empty()) ttResize(max(1, min(8192, stoi(value))));
            else if (name == "Clear Hash") ttClear();
            else if (name == "Threads" && !value.empty()) setThreads(max(1, min(256, stoi(value))));
            else if (name == "EvalFile" && !value.empty() && value != "<empty>") tryLoadNet(value, true);
            else if (name == "NNUE Blend" && !value.empty()) { try { nnueBlend = max(0, min(100, stoi(value))); } catch (...) {} }
            else if (name == "Use NNUE") { if (value == "false") NN::loaded = false; else { if (NN::loadedName.empty()) say("info string set EvalFile first"); else tryLoadNet(NN::loadedName, true); } }
        }
        else if (cmd == "position") { stopSearch(); setPosition(ss); }
        else if (cmd == "go") { stopSearch(); go(ss); }
        else if (cmd == "stop") stopSearch();
        else if (cmd == "ponderhit") {
            if (pondering) { startNs = nowNs(); timeLimited = ponderTimeLimited; pondering = false; }
        }
        else if (cmd == "quit") { stopSearch(); break; }
        else if (cmd == "perft") {
            int d = 5; ss >> d; auto t0 = chrono::steady_clock::now();
            U64 n = perft(rootPos, d);
            int64_t ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();
            say("perft " + to_string(d) + ": " + to_string(n) + " (" + to_string(ms) + " ms)");
        }
        else if (cmd == "datagen") {
            // datagen <games> <soft nodes per move> <output file> <seed> [random opening plies]
            int g = 1000, n = 5000, rp = 8; string f = "data.bin"; U64 sd = 1; ss >> g >> n >> f >> sd; if (ss >> rp) dgRandomPlies = max(0, min(20, rp));
            datagen(g, n, f, sd * 0x9E3779B97F4A7C15ULL + 12345);
        }
        else if (cmd == "texel") {
            int iters = 300; double lam = 0.5; size_t mx = 3000000; string out = "tuned.txt", f; vector<string> fs;
            ss >> iters >> lam >> mx >> out; while (ss >> f) fs.push_back(f);
            texelTune(iters, lam, out, fs, mx);
        }
        else if (cmd == "eval") {
            say("classical eval (side to move): " + to_string(evaluate(rootPos)));
            if (NN::loaded) { NN::Acc a; NN::refreshAll(a, rootPos); say("NNUE eval (side to move): " + to_string(NN::evaluate(a, rootPos.side, popc(rootPos.all)))); }
        }
        else if (cmd == "bench") { stopSearch(); int d = 12; ss >> d; bench(d); }
    }
    stopSearch();
    return 0;
}

#endif // native main
#ifdef LANTERN_BROWSER
#include <sstream>
#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE __attribute__((used))
#endif
static bool webInitialized = false;
static string webReturn;
static string jsonEscape(const string &s) {
    string out;
    for (char c : s) {
        if (c == '\"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if ((unsigned char)c < 32) out += " ";
        else out += c;
    }
    return out;
}
static void webInit() {
    if (webInitialized) return;
    initAttacks(); initZobrist(); initEval(); initEvalParams(); applyTunedParams();
    for (int d = 0; d < 64; d++) for (int m = 0; m < 64; m++) lmrTab[d][m] = lmrInit(d, m);
#ifdef LANTERN_BROWSER_PTHREADS
    ttResize(512); setThreads(3);  // Full native 3-thread/512-MB settings; needs COOP/COEP hosting.
#else
    ttResize(32); setThreads(1);    // Compatible with ordinary static web hosting.
#endif
    rootPos.setFen(STARTFEN);
    gameKeyN = 0; gameKeys[gameKeyN++] = rootPos.key;
    nnueBlend = 50;
    webInitialized = true;
}
static bool validWebFen(const string &fen) {
    istringstream ss(fen); string board, turn, castling, ep; int half = 0, full = 1;
    if (!(ss >> board >> turn >> castling >> ep >> half >> full)) return false;
    if (turn != "w" && turn != "b") return false;
    if (half < 0 || full < 1 || full > 100000) return false;
    if (castling != "-") for (char c : castling) if (string("KQkq").find(c) == string::npos) return false;
    if (ep != "-" && (ep.size() != 2 || ep[0] < 'a' || ep[0] > 'h' || (ep[1] != '3' && ep[1] != '6'))) return false;
    int row = 0, cols = 0, kingsW = 0, kingsB = 0;
    for (char c : board) {
        if (c == '/') { if (cols != 8) return false; row++; cols = 0; }
        else if (c >= '1' && c <= '8') cols += c - '0';
        else if (string("PNBRQKpnbrqk").find(c) != string::npos) {
            cols++; if (c == 'K') kingsW++; else if (c == 'k') kingsB++;
        } else return false;
        if (cols > 8) return false;
    }
    return row == 7 && cols == 8 && kingsW == 1 && kingsB == 1;
}
static string fenAfter(const Pos &p, int fullmove) {
    const string chars = "PNBRQKpnbrqk";
    string f;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            int pc = p.bd[r*8+file];
            if (pc == EMPTY) { empty++; continue; }
            if (empty) { f += char('0'+empty); empty=0; }
            f += chars[pc];
        }
        if (empty) f += char('0'+empty);
        if (r) f += '/';
    }
    f += p.side == WHITE ? " w " : " b ";
    if (!p.castle) f += "-";
    else { if(p.castle & 1) f+='K'; if(p.castle & 2) f+='Q'; if(p.castle & 4) f+='k'; if(p.castle & 8) f+='q'; }
    f += ' ';
    if (p.ep < 0) f += '-';
    else f += char('a'+p.ep%8), f += char('1'+p.ep/8);
    f += ' ' + to_string(p.fifty) + ' ' + to_string(fullmove);
    return f;
}
extern "C" {
EMSCRIPTEN_KEEPALIVE const char *lantern_web_init() {
    webInit();
    bool ok = NN::load("/nets/full_best.nnue");
    nnueBlend = 50;
    webReturn = ok ? "OK: Lantern 16.4 with full_best.nnue H50" : "ERROR: Could not load /nets/full_best.nnue";
    return webReturn.c_str();
}
EMSCRIPTEN_KEEPALIVE const char *lantern_web_state(const char *fenText) {
    webInit();
    string fen = fenText ? fenText : "";
    if (!validWebFen(fen)) { webReturn = "{\"error\":\"Invalid FEN\"}"; return webReturn.c_str(); }
    Pos p; p.setFen(fen);
    MoveList ml; genMoves(p, ml, false);
    string moves; int cnt=0;
    for (int i=0; i<ml.n; ++i) {
        Pos copy=p;
        if (copy.make(ml.mv[i])) {
            if (cnt++) moves += ',';
            moves += '\"' + moveStr(ml.mv[i]) + '\"';
        }
    }
    bool check = p.inCheck();
    bool mate = check && cnt == 0;
    bool stalemate = !check && cnt == 0;
    bool draw = insufficient(p) || p.fifty >= 150;
    webReturn = "{\"moves\":["+moves+"],\"check\":"+(check?"true":"false")+
        ",\"mate\":"+(mate?"true":"false")+
        ",\"stalemate\":"+(stalemate?"true":"false")+
        ",\"draw\":"+(draw?"true":"false")+"}";
    return webReturn.c_str();
}
EMSCRIPTEN_KEEPALIVE const char *lantern_web_move(const char *fenText, const char *moveText) {
    webInit();
    string fen=fenText?fenText:"", move=moveText?moveText:"";
    if (!validWebFen(fen)) { webReturn="ERROR: Invalid FEN"; return webReturn.c_str(); }
    Pos p; p.setFen(fen);
    int m=parseMove(p, move);
    if (!m) { webReturn="ERROR: Illegal move"; return webReturn.c_str(); }
    int full=1;
    istringstream ss(fen); string tok; for(int i=0;i<5;i++) ss>>tok; ss>>full;
    int mover = p.side; p.make(m);
    webReturn=fenAfter(p, full+(mover==BLACK));
    return webReturn.c_str();
}
EMSCRIPTEN_KEEPALIVE const char *lantern_web_search(const char *fenText, int milliseconds, int blend) {
    webInit();
    string fen=fenText?fenText:"";
    if (!validWebFen(fen)) { webReturn="ERROR: Invalid FEN"; return webReturn.c_str(); }
    nnueBlend=max(0,min(100,blend));
    stopSearch();
    istringstream ss("fen " + fen); setPosition(ss);
    // Search with the exact native Lantern PVS/NNUE implementation.
    // Single-thread compilation avoids cross-origin isolation requirements.
    istringstream args("movetime " + to_string(max(30,min(milliseconds,30000))));
    ostringstream captured; streambuf *original=cout.rdbuf(captured.rdbuf());
    try {
        go(args);
#ifdef LANTERN_BROWSER_PTHREADS
        waitSearch();  // Join helper search thread before restoring captured UCI output.
#endif
    } catch (...) { cout.rdbuf(original); webReturn="ERROR: Search failed"; return webReturn.c_str(); }
    cout.rdbuf(original);
    webReturn=captured.str();
    return webReturn.c_str();
}
} // extern C
#endif // LANTERN_BROWSER
