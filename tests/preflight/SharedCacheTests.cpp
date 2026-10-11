#include "preflight/SharedCache.h"
#include "preflight/LuaPreflight.h"
#include <atomic>
#include <barrier>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
using namespace sipi::preflight;
using namespace std::chrono_literals;
void check(bool ok) { if (!ok) throw std::runtime_error("Cache assertion failed"); }
template<class F> void rejects(F&& f) { bool failed=false; try { f(); } catch (...) { failed=true; } check(failed); }
auto deadline() { return SharedCache::Clock::now()+2s; }

void native_tests() {
    std::atomic<int> ticks=0;
    SharedCache cache({2,100}, [&]{ return SharedCache::Clock::time_point{}+std::chrono::seconds(ticks.load()); });
    auto load=[] { return SharedCache::Loaded{"value",10s}; };
    auto first=cache.get_or_load("key",load,deadline());
    check(cache.get("key")->bytes=="value");
    ticks=10;
    check(!cache.get("key")); // Exact expiration, not one clock tick later.
    auto second=cache.get_or_load("key",load,deadline());
    check(second.revision!=first.revision && !cache.invalidate("key",first.revision));
    check(cache.invalidate("key",second.revision));
    rejects([&]{ cache.get_or_load("failure",[]{throw std::runtime_error("private"); return SharedCache::Loaded{};},deadline()); });
    check(!cache.get("failure"));
    check(cache.get_or_load("failure",load,deadline()).bytes=="value");
    rejects([&]{cache.get_or_load("large",[]{return SharedCache::Loaded{std::string(101,'x'),1s};},deadline());});
    auto transient=cache.get_or_load("zero",[]{return SharedCache::Loaded{"x",0s};},deadline());
    check(transient.bytes=="x" && !cache.get("zero"));
    check(!cache.invalidate("zero",transient.revision));
    check(cache.get_or_load("zero",load,deadline()).bytes=="value");
    rejects([&]{cache.get_or_load("negative",[]{return SharedCache::Loaded{"x",-1s};},deadline());});
    rejects([&]{cache.get_or_load("slow",[&]{ticks+=11; return load();},deadline());});
    check(!cache.get("slow"));
    ticks=30; cache.get_or_load("a",load,deadline());
    ticks=31; cache.get_or_load("b",load,deadline());
    ticks=32; (void)cache.get("a");
    ticks=33; cache.get_or_load("c",load,deadline());
    check(cache.get("a") && !cache.get("b") && cache.get("c"));

    SharedCache shared;
    std::atomic<int> calls=0;
    std::barrier start(12);
    std::vector<std::future<SharedCache::Value>> futures;
    for(int i=0;i<12;++i) futures.push_back(std::async(std::launch::async,[&]{
        start.arrive_and_wait();
        return shared.get_or_load("same",[&]{++calls; std::this_thread::sleep_for(50ms);return load();},deadline());
    }));
    for(auto& f:futures) check(f.get().bytes=="value");
    check(calls==1);
    calls=0; futures.clear();
    for(int i=0;i<12;++i) futures.push_back(std::async(std::launch::async,[&]{
        start.arrive_and_wait();
        return shared.get_or_load("transient",[&]{++calls; std::this_thread::sleep_for(100ms);
            return SharedCache::Loaded{"not retained",0s};},deadline());
    }));
    for(auto& f:futures) check(f.get().bytes=="not retained");
    check(calls==1 && !shared.get("transient"));
    std::promise<void> entered, release;
    auto gate=release.get_future();
    auto owner=std::async(std::launch::async,[&]{ return shared.get_or_load("wait",[&]{entered.set_value(); gate.wait();return load();},deadline()); });
    entered.get_future().wait();
    check(!shared.get("wait"));
    bool timed_out = false;
    try { shared.get_or_load("wait",load,SharedCache::Clock::now()+10ms); }
    catch (const std::runtime_error& error) {
        timed_out = std::string(error.what()) == "Cache loader wait deadline exceeded";
    }
    check(shared.get_or_load("independent",load,deadline()).bytes=="value");
    release.set_value(); check(owner.get().bytes=="value");
    check(timed_out);
    SharedCache small({1,16});
    std::promise<void> filling, finish;
    auto finish_gate=finish.get_future();
    auto active=std::async(std::launch::async,[&]{return small.get_or_load("active",[&]{
        filling.set_value(); finish_gate.wait(); return load();},deadline());});
    filling.get_future().wait();
    rejects([&]{small.get_or_load("other",load,deadline());});
    finish.set_value(); check(active.get().bytes=="value");
    check(small.get_or_load("other",load,deadline()).bytes=="value");
    check(!small.get("active"));

    // Failed owners must wake waiters without retaining either a value or an error.
    std::promise<void> failing, fail_now, waiter_entered;
    auto failure_gate=fail_now.get_future();
    auto failed_owner=std::async(std::launch::async,[&]{rejects([&]{shared.get_or_load("failure",[&]() -> SharedCache::Loaded {
        failing.set_value(); failure_gate.wait(); throw std::runtime_error("private failure");},deadline());});});
    failing.get_future().wait();
    auto waiter=std::async(std::launch::async,[&]{waiter_entered.set_value(); rejects([&]{
        shared.get_or_load("failure",[]() -> SharedCache::Loaded {throw std::runtime_error("failed again");},deadline());});});
    waiter_entered.get_future().wait();
    fail_now.set_value(); failed_owner.get();
    check(waiter.wait_for(500ms)==std::future_status::ready); waiter.get();
    check(shared.get_or_load("failure",load,deadline()).bytes=="value");
}

void lua_tests(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto path=directory/"cache.lua";
    std::ofstream(path) << R"lua(
      assert(not pcall(function() sipi.cache.get('x') end))
      finalizer=setmetatable({}, {__gc=function()
        pcall(function() sipi.cache.get_or_load('finalizer',function() return 'unsafe',60 end) end)
      end})
      function preflight(r)
        assert(sipi.cache.get('finalizer')==nil)
        local value, rev = sipi.cache.get_or_load('shared', function()
          return {items={1,2}, empty=sipi.json.array(), null=sipi.json.null}, 60
        end)
        assert(value.items[1]==1 and #value.empty==0 and value.null==sipi.json.null)
        value.items[1]=999
        assert(sipi.cache.get('shared').items[1]==1)
        assert(not sipi.cache.invalidate('shared',rev+1))
        local no, generation=sipi.cache.get('missing'); assert(no==nil and generation==nil)
        local transient, transient_rev=sipi.cache.get_or_load('transient',function() return {decision='deny'},0 end)
        assert(transient.decision=='deny' and sipi.cache.get('transient')==nil)
        assert(not sipi.cache.invalidate('transient',transient_rev))
        local false_value=sipi.cache.get_or_load('false',function() return false,60 end)
        assert(false_value==false)
        assert(not pcall(function() sipi.cache.get_or_load('recursive',function()
          return sipi.cache.get_or_load('nested',function() return 'x',60 end)
        end) end))
        assert(sipi.cache.get('recursive')==nil)
        local ok, message = pcall(function()
          sipi.cache.get_or_load('bad',function() error('loader diagnostic marker') end)
        end)
        assert(not ok and message:find('loader diagnostic marker',1,true))
        local marker = {}
        local object_ok, object = pcall(function()
          sipi.cache.get_or_load('object-error',function() error(marker) end)
        end)
        assert(not object_ok and object == marker)
        local ttl_ok, ttl_error = pcall(function()
          sipi.cache.get_or_load('bad-ttl',function() return 'value',3601 end)
        end)
        assert(not ttl_ok and ttl_error:find('Cache TTL must be in 0..3600 seconds',1,true))
        assert(sipi.cache.get_or_load('bad',function() return 'recovered',60 end)=='recovered')
        local _, v=sipi.cache.get('bad'); assert(sipi.cache.invalidate('bad',v))
        for _, loader in ipairs({function() return nil,1 end, function() return {},-1 end,
                                 function() return function() end,1 end,
                                 function() return string.rep('x',65537),1 end,
                                 function() return 'x',1,'extra' end}) do
          assert(not pcall(function() sipi.cache.get_or_load('invalid',loader) end))
        end
        sipi.cache.get_or_load('memory',function() return string.rep('x',60000),60 end)
        local held={}
        pcall(function() while true do held[#held+1]=string.rep('y',65536) end end)
        pcall(function() sipi.cache.get('memory') end)
        pcall(function() sipi.cache.get_or_load('memory-load',function() return string.rep('q',60000),60 end) end)
        held=nil; collectgarbage('collect')
        assert(#sipi.cache.get('memory')==60000)
        assert(#sipi.cache.get_or_load('memory-load',function() return string.rep('q',60000),60 end)==60000)
        return {decision='allow'}
      end
    )lua";
    LuaPreflight policy(path);
    policy.validate();
    for(int i=0;i<3;++i) check(std::holds_alternative<Allow>(policy.evaluate({{{},"id"},{},{}})));
    // A separate policy snapshot must not inherit values, even for the same file.
    std::ofstream(path) << "function preflight(r) assert(sipi.cache.get('shared')==nil); return {decision='allow'} end";
    LuaPreflight isolated(path);
    check(std::holds_alternative<Allow>(isolated.evaluate({{{},"id"},{},{}})));
}
int main(int argc,char** argv) {
    try { check(argc==2); native_tests(); lua_tests(argv[1]); std::cout<<"Shared cache and Lua checks passed\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
