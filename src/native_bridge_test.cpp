#define LANTERN_BROWSER 1
#include "lantern_web.cpp"
#include <cassert>
int main(){
    auto s=string(lantern_web_init());
    fprintf(stderr,"init: %s\n",s.c_str());
    string start="rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    string st=lantern_web_state(start.c_str());
    fprintf(stderr,"state=%s\n",st.c_str());
    assert(st.find("\"e2e4\"") != string::npos);
    string next=lantern_web_move(start.c_str(),"e2e4");
    fprintf(stderr,"after e2e4=%s\n",next.c_str());
    assert(next=="rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    assert(string(lantern_web_move(next.c_str(),"e7e5"))=="rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2");
    bool loaded=NN::load("/mnt/data/full_best.nnue");
    assert(loaded && NN::buckets==8 && NN::HIDDEN==1024);
    string result=lantern_web_search(start.c_str(),110,50);
    fprintf(stderr,"search log:\n%s\n",result.c_str());
    assert(result.find("bestmove ") != string::npos);
    assert(string(lantern_web_state("definitely-invalid"))=="{\"error\":\"Invalid FEN\"}");
    string castle = lantern_web_move("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1");
    assert(castle=="r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1");
    string promotion = lantern_web_move("4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8n");
    assert(promotion=="N3k3/8/8/8/8/8/8/4K3 b - - 0 1");
    string mate="rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3";
    auto mateState=string(lantern_web_state(mate.c_str()));
    fprintf(stderr,"mate state=%s\n",mateState.c_str());
    assert(mateState.find("\"mate\":true")!=string::npos);
    assert(string(lantern_web_search(mate.c_str(),110,50)).find("bestmove 0000")!=string::npos);
    fprintf(stderr,"PASS native bridge with REAL full_best.nnue, legal moves, castling, promotion, checkmate and search\n");
}
