#ifndef PLEXGLOBALTIMER_H
#define PLEXGLOBALTIMER_H

#include "threads/Timer.h"
#include "threads/CriticalSection.h"
#include "utils/JobManager.h"

#include <boost/shared_ptr.hpp>

class IPlexGlobalTimeout
{
  public:
    virtual ~IPlexGlobalTimeout() {}
    virtual void OnTimeout() = 0;
    virtual std::string TimerName() const { return "unnamed"; }
};

typedef std::pair<int64_t, IPlexGlobalTimeout*> timeoutPair;

class CPlexGlobalTimerJob : public CJob
{
  public:
    CPlexGlobalTimerJob(IPlexGlobalTimeout* callback) : m_callback(callback) {}

    bool DoWork()
    {
      m_callback->OnTimeout();
      return true;
    }

    IPlexGlobalTimeout* m_callback;
};

class CPlexGlobalTimer : public CThread
{
  public:
    CPlexGlobalTimer() : CThread("CPlexGlobalTimer"), m_running(false) { Create(); }
    ~CPlexGlobalTimer();
    void SetTimeout(int64_t msec, IPlexGlobalTimeout* callback);
    void RemoveTimeout(IPlexGlobalTimeout* callback);
    void RestartTimeout(int64_t msec, IPlexGlobalTimeout* callback);
    void RemoveAllTimeoutsByName(const std::string& name);

    void StopAllTimers();
  private:
    void Process();
    void SetTimer();
    void DumpDebug();

    CCriticalSection m_timerLock;
    std::vector<timeoutPair> m_timeouts;
    CEvent m_timerEvent;
    bool m_running;
};

typedef boost::shared_ptr<CPlexGlobalTimer> CPlexGlobalTimerPtr;

#endif // PLEXGLOBALTIMER_H
