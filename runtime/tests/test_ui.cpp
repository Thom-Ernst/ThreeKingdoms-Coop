#include "../src/ui_protocol.h"
#include "../src/ui.h"
#include <cassert>
#include <cstdio>
#include <map>
struct Fake : twui::Backend {
    std::map<twui::Widget, twui::Node> nodes{{1,{"root","active","",true,false}},
        {2,{"menu","active","hello\tworld\n\\",true,false}},
        {3,{"button","active","",true,false}}, {4,{"button","active","",true,false}}};
    std::map<twui::Widget,std::vector<twui::Widget>> kids{{1,{2}}, {2,{3}}, {3,{}}, {4,{}}};
    int clicks = 0, reads = 0, quits = 0, texts = 0, keys = 0;
    std::vector<twui::Widget> rootWidgets{1};
    bool roots(std::vector<twui::Widget>& out,std::string&) override { ++reads; out=rootWidgets; return true; }
    bool text(twui::Widget, const std::string&, std::string&) override { ++texts; return true; }
    bool quit(std::string&) override { ++quits; return true; }
    bool root(twui::Widget& w,std::string&) override { ++reads; w=1; return true; }
    bool node(twui::Widget w,twui::Node& n) override { ++reads; if (!nodes.count(w)) return false; n=nodes[w]; return true; }
    bool children(twui::Widget w,std::vector<twui::Widget>& out) override { out=kids[w]; return true; }
    bool click(twui::Widget,std::string&) override { ++clicks; return true; }
    bool key(const std::string& key,std::string&) override { assert(key=="esc"); ++keys; return true; }
};
int main(int argc,char** argv) {
    assert(argc==2 && SetCurrentDirectoryA(argv[1])); // Only the isolated fixture directory.
    assert(!uiArmingFlagExists());
    assert(CreateDirectoryA("tw3k_ui_test.flag",nullptr));
    assert(!uiArmingFlagExists()); // A directory is not an arming file.
    assert(RemoveDirectoryA("tw3k_ui_test.flag"));
    auto flag=CreateFileA("tw3k_ui_test.flag",GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    assert(flag!=INVALID_HANDLE_VALUE && CloseHandle(flag));
    assert(uiArmingFlagExists());
    assert(DeleteFileA("tw3k_ui_test.flag") && !uiArmingFlagExists());
    twui::Request r; std::string error; std::vector<std::string> parts;
    assert(twui::pathParts("/",parts) && parts.empty());
    assert(twui::pathParts("/a/menu with spaces",parts) && parts.size()==2);
    for (auto p : {"", "a", "/a/", "/a//b", "/a/..", "/a/.", "/a/*", "/a?", "/a\\b", "/a\nb"}) assert(!twui::pathParts(p,parts));
    assert(twui::parse("ui\ttree\t/menu with spaces\t8",r,error) && r.path=="/menu with spaces" && r.depth==8);
    assert(twui::parse("ui text /game A name",r,error) && r.value=="A name");
    assert(twui::parse("ui key esc",r,error) && r.value=="esc");
    assert(twui::parse("ui\tkey\tesc",r,error) && r.value=="esc");
    for (auto c : {"ui key", "ui key ESC", "ui key enter", "ui key esc extra"}) assert(!twui::parse(c,r,error));
    assert(twui::parse("ui\ttext\t/game\t",r,error)==false); // Empty interactive text is not exposed.
    for (auto c : {"ui tree / 9", "ui tree / -1", "ui tree / 1x", "ui click / extra", "ui arm extra", "ui quit extra", "ui key / UP", "ui click /\n"}) assert(!twui::parse(c,r,error));
    assert(twui::playerRow(19,"record","cached")=="player\t19\trecord\n");
    assert(twui::playerRow(4,"","captured\tname")=="player\t4\tcaptured\\tname\n");
    assert(twui::playerRow(31,"","")=="player\t31\n");
    Fake b; bool armed=false;
    auto run=[&](const char* c,bool flag=false) { assert(twui::parse(c,r,error)); return twui::execute(r,b,armed,flag); };
    assert(run("ui status")=="ok disarmed\n");
    assert(run("ui click /menu/button")=="fail disarmed\n" && b.reads==0 && b.clicks==0);
    assert(run("ui quit")=="fail disarmed\n" && b.quits==0);
    assert(run("ui key esc")=="fail disarmed\n" && b.keys==0 && b.reads==0);
    assert(run("ui arm")=="fail arming-flag-missing\n" && !armed);
    assert(run("ui arm",true)=="ok armed\n" && armed);
    assert(run("ui quit")=="ok close-posted\n" && b.quits==1 && b.reads==0);
    b.rootWidgets.clear();
    assert(run("ui key esc")=="ok\n" && b.keys==1 && b.reads==0); // Root teardown never blocks Esc.
    b.rootWidgets={1};
    assert(run("ui tree / 1")=="/\t1\t0\tactive\t\n/menu\t1\t0\tactive\thello\\tworld\\n\\\\\nok\n");
    assert(run("ui modal")=="/menu\t1\t0\tactive\thello\\tworld\\n\\\\\tmenu\nok\n");
    assert(run("ui click /menu/button")=="ok\n" && b.clicks==1);
    assert(run("ui click /button")=="fail not-found\n"); // No recursive search.
    assert(run("ui click /Menu/button")=="fail not-found\n"); // Exact case.
    assert(run("ui find root")=="ok\n"); // The root Id is omitted from absolute paths.
    // Unaddressable direct children stay discoverable, including their escaped raw Id.
    for (auto id : {"", "bad/id", "bad\\id", "bad*id", "bad?id", "bad\tid", "bad\nid", "bad\x01id", "bad\x7Fid", ".", ".."}) {
        b.nodes[2].id=id;
        auto tree=run("ui tree / 2");
        assert(tree.find("/#0\t1\t0\tactive\t")!=std::string::npos);
        assert(tree.find("\t"+twui::escaped(id)+"\n")!=std::string::npos);
        assert(run("ui find button").find("/#0/button\t")!=std::string::npos);
        assert(run("ui click /#0/button")=="ok\n");
        assert(run("ui text /#0 test")=="ok\n");
    }
    b.nodes[2].id="bad/id";
    assert(run("ui find bad/id").find("/#0\t")!=std::string::npos);
    b.nodes[4].id="#0"; b.kids[1]={2,4};
    assert(run("ui click /#0")=="fail ambiguous-path\n");
    b.kids[4]={3};
    assert(run("ui click /#0/button")=="fail ambiguous-path\n");
    b.kids[1]={4}; b.kids[4]={};
    assert(run("ui click /#0")=="ok\n"); // A literal #0 alone remains addressable.
    b.nodes[4].id="bad/id";
    assert(run("ui click /#1")=="fail not-found\n"); // No invented index.
    b.nodes[4].id="button"; b.nodes[2].id="menu"; b.kids[1]={2}; b.clicks=1;
    b.kids[2]={3,4};
    assert(run("ui click /menu/button")=="fail ambiguous-path\n" && b.clicks==1);
    auto all=run("ui find button");
    assert(all.find("/menu/button\t")!=std::string::npos && all.rfind("ok\n")==all.size()-3);
    b.kids[2]={3}; b.nodes[3].visible=false;
    assert(run("ui click /menu/button")=="fail hidden\n" && b.clicks==1);
    b.nodes[3].visible=true; b.nodes[3].disabled=true;
    assert(run("ui click /menu/button")=="fail disabled\n" && b.clicks==1);
    b.nodes[3].disabled=false; b.kids[3]={1};
    assert(run("ui find button").find("fail cyclic-or-shared-child")!=std::string::npos);
    b.kids[3]={999};
    assert(run("ui click /menu/button/x")=="fail invalid-or-cyclic-child\n");
    b.kids[3]={}; b.nodes[2].text=std::string(twui::MaxReply,'x');
    assert(run("ui tree / 2").find("fail reply-limit")!=std::string::npos);
    b.nodes[2].text=""; b.kids[1]=std::vector<twui::Widget>(2001,2);
    assert(run("ui tree / 1").find("fail node-limit")!=std::string::npos);
    assert(run("ui arm")=="fail arming-flag-missing\n" && !armed);
    assert(run("ui arm",true)=="ok armed\n");
    assert(run("ui disarm")=="ok disarmed\n" && !armed);
    // ★ A forest is explicitly addressed; identical names across roots never choose a seat/panel.
    Fake multi; bool multiArmed=true;
    multi.nodes[10]={"popup\troot","active","",true,false};
    multi.nodes[11]={"menu","selected","popup text",true,true};
    multi.nodes[12]={"nested","active","",true,false};
    multi.nodes[13]={"hidden","active","",false,false};
    multi.kids[10]={11,13}; multi.kids[11]={12};
    multi.kids[12]={}; multi.kids[13]={}; multi.rootWidgets={1,10};
    auto forest=[&](const char* c) { assert(twui::parse(c,r,error)); return twui::execute(r,multi,multiArmed,true); };
    assert(forest("ui roots")=="root\t/@0\troot\t1\nroot\t/@1\tpopup\\troot\t2\nok\n");
    assert(forest("ui modal")=="/@0/menu\t1\t0\tactive\thello\\tworld\\n\\\\\tmenu\n/@1/menu\t1\t1\tselected\tpopup text\tmenu\nok\n");
    assert(forest("ui find menu").find("/@1/menu\t1\t1\tselected")!=std::string::npos);
    assert(forest("ui click /menu/button")=="fail ambiguous-root-use-/@N\n" && multi.clicks==0);
    assert(forest("ui click /@0/menu/button")=="ok\n" && multi.clicks==1);
    assert(forest("ui click /@1/menu")=="fail disabled\n" && multi.clicks==1);
    assert(forest("ui tree /@1 0").find("/@1\t1\t0\tactive") == 0);
    assert(forest("ui tree / 1").find("/@0/menu\t")!=std::string::npos);
    for (auto c : {"ui tree /@2", "ui tree /@01", "ui tree /@-1", "ui tree /@999999999999999999999"})
        assert(forest(c)=="fail invalid-root-selector\n");
    multi.nodes[11].id="bad/id";
    assert(forest("ui modal").find("/@1/#0\t1\t1\tselected\tpopup text\tbad/id\n")!=std::string::npos);
    multi.kids[10]={11,11};
    assert(forest("ui modal").find("fail cyclic-or-shared-child")!=std::string::npos);
    multi.kids[10]={11,2};
    assert(forest("ui modal").find("fail cyclic-or-shared-child")!=std::string::npos);
    multi.kids[10]={999};
    assert(forest("ui modal").find("fail invalid-child")!=std::string::npos);
    multi.kids[10]={11}; multi.nodes[11].text=std::string(twui::MaxReply,'x');
    assert(forest("ui modal").find("fail reply-limit")!=std::string::npos);
    multi.rootWidgets={1,1};
    assert(forest("ui roots")=="fail invalid-or-duplicate-root\n");
    multi.rootWidgets={};
    assert(forest("ui modal")=="fail no-live-widget-root-source\n");
    multi.rootWidgets=std::vector<twui::Widget>(twui::MaxNodes+1,1);
    assert(forest("ui roots")=="fail node-limit\n");
    multi.rootWidgets={1};
    multi.nodes[2].id="";
    assert(forest("ui modal").find("/#0\t1\t0\tactive\thello\\tworld\\n\\\\\t\n") == 0);
    multi.nodes[2].id="menu";
    assert(forest("ui click /@0/menu/button")=="ok\n");
    multi.nodes[2].id="@0";
    assert(forest("ui click /@0")=="ok\n"); // Existing frontend literal path survives.
    assert(forest("ui roots")=="fail root-selector-collides-with-legacy-id\n");
    multi.nodes[2].id="@panel";
    assert(forest("ui click /@panel/button")=="ok\n");
    multiArmed=false;
    assert(forest("ui roots")=="fail disarmed\n" && forest("ui modal")=="fail disarmed\n");
    for (auto c : {"ui roots extra", "ui modal /"}) assert(!twui::parse(c,r,error));
    assert(twui::fail("bad\nreason")=="fail bad\\nreason\n");
    puts("ok actual UI parser, working-directory flag/file rule, arming, paths, ambiguity, visibility, disabled, limits, replies");
}
