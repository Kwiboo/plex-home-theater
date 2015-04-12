#include "PlexBusyIndicator.h"
#include "utils/JobManager.h"
#include "dialogs/GUIDialogBusy.h"
#include "guilib/GUIWindowManager.h"
#include "boost/foreach.hpp"
#include "utils/log.h"
#include "Application.h"
#include "PlexJobs.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
CPlexBusyIndicator::CPlexBusyIndicator()
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexBusyIndicator::blockWaitingForJob(CJob* job, IJobCallback* callback, CFileItemListPtr *result)
{
  CSingleLock lk(m_section);
  m_blockEvent.Reset();
  int id = CJobManager::GetInstance().AddJob(job, this);

  m_callbackMap[id] = callback;
  m_resultMap[id] = result;

  lk.Leave();

  bool success = true;
  if (!CGUIDialogBusy::WaitOnEvent(m_blockEvent, 300))
  {
    lk.Enter();
    std::pair<int, IJobCallback*> p;
    BOOST_FOREACH(p, m_callbackMap)
      CJobManager::GetInstance().CancelJob(p.first);

    // Let's get out of here.
    m_callbackMap.clear();
    m_resultMap.clear();
    m_blockEvent.Set();
    success = false;
  }

  return success;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void CPlexBusyIndicator::OnJobComplete(unsigned int jobID, bool success, CJob* job)
{
  CSingleLock lk(m_section);

  if (m_callbackMap.find(jobID) != m_callbackMap.end())
  {
    IJobCallback* cb = m_callbackMap[jobID];
    if (cb)
      cb->OnJobComplete(jobID, success, job);

    if (m_resultMap[jobID])
    {
      CPlexJob *plexjob = dynamic_cast<CPlexJob*>(job);
      if (plexjob)
      {
        *m_resultMap[jobID] = plexjob->getResult();
      }
    }

    m_callbackMap.erase(jobID);
    m_resultMap.erase(jobID);

    if (m_callbackMap.size() == 0)
    {
      CLog::Log(LOGDEBUG, "CPlexBusyIndicator::OnJobComplete nothing more blocking, let's leave");
      m_blockEvent.Set();
    }
  }
  else
  {
    CLog::Log(LOGDEBUG, "CPlexBusyIndicator::OnJobComplete ouch, we got %d that we don't have a callback for?", jobID);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void CPlexBusyIndicator::OnJobProgress(unsigned int jobID, unsigned int progress,
                                       unsigned int total, const CJob* job)
{
  CSingleLock lk(m_section);

  if (m_callbackMap.find(jobID) != m_callbackMap.end())
  {
    IJobCallback* cb = m_callbackMap[jobID];
    lk.Leave();
    if (cb)
      cb->OnJobProgress(jobID, progress, total, job);
  }
  else
  {
    CLog::Log(LOGDEBUG, "CPlexBusyIndicator::OnJobProgress ouch, we got %d that we don't have a callback for?", jobID);
  }
}
