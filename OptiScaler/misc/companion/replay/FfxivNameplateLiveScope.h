#pragma once
namespace FfxivNameplateLiveScope
{
inline thread_local bool active=false;
struct Guard { bool previous=active; Guard(){active=true;} ~Guard(){active=previous;} };
}
