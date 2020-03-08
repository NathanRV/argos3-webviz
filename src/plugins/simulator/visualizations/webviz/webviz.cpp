/**
 * @file <argos3/plugins/simulator/visualizations/webviz/webviz.cpp>
 *
 * @author Prajankya Sonar - <prajankya@gmail.com>
 *
 * MIT License
 * Copyright (c) 2020 NEST Lab
 */

#include "webviz.h"

namespace argos {

  /****************************************/
  /****************************************/

  CWebviz::CWebviz()
      : m_eExperimentState(Webviz::EExperimentState::EXPERIMENT_INITIALIZED),
        m_cTimer(),
        m_cSimulationThread(
          std::thread(&CWebviz::SimulationThreadFunction, this)),
        m_cSpace(m_cSimulator.GetSpace()),
        m_bFastForwarding(false) {}

  /****************************************/
  /****************************************/

  void CWebviz::Init(TConfigurationNode& t_tree) {
    unsigned short unPort;
    unsigned short unBroadcastFrequency;

    /* Parse options from the XML */
    GetNodeAttributeOrDefault(t_tree, "port", unPort, UInt16(3000));
    GetNodeAttributeOrDefault(
      t_tree, "broadcast_frequency", unBroadcastFrequency, UInt16(10));

    if (unBroadcastFrequency < 1 || 10 < unBroadcastFrequency) {
      throw CARGoSException(
        "Broadcast frequency set in configuration is out of range [1,1000]");
      return;  // just for readability
    }
    GetNodeAttributeOrDefault(
      t_tree, "ff_draw_frames_every", m_unDrawFrameEvery, UInt16(2));

    /* Initialize Webserver */
    m_cWebServer = new Webviz::CWebServer(this, unPort, unBroadcastFrequency);

    /* Write all the pending stuff */
    LOG.Flush();
    LOGERR.Flush();

    /* Disable Colors in LOG, as its going to be shown in web and not in CLI */
    LOG.DisableColoredOutput();
    LOGERR.DisableColoredOutput();

    /* Initialize the LOG streams from Execute thread */
    m_pcLogStream =
      new Webviz::CLogStream(LOG.GetStream(), [this](std::string str_logData) {
        m_cWebServer->EmitLog("LOG", str_logData);
      });

    m_pcLogErrStream = new Webviz::CLogStream(
      LOGERR.GetStream(), [this](std::string str_logData) {
        m_cWebServer->EmitLog("LOGERR", str_logData);
      });
  }

  /****************************************/
  /****************************************/

  void CWebviz::Execute() {
    std::thread t2([&]() {
      /* Set up thread-safe buffers for this new thread */
      LOG.AddThreadSafeBuffer();
      LOGERR.AddThreadSafeBuffer();

      m_cWebServer->Start();
    });

    t2.join();
    m_cSimulationThread.join();

    LOG.Flush();
    LOGERR.Flush();
    // TODO Finish all..
  }

  /* main simulation thread */
  void CWebviz::SimulationThreadFunction() {
    /* Set up thread-safe buffers for this new thread */
    LOG.AddThreadSafeBuffer();
    LOGERR.AddThreadSafeBuffer();

    /* Fast forward steps counter used inside */
    int unFFStepCounter = 1;

    while (true) {
      if (
        m_eExperimentState == Webviz::EExperimentState::EXPERIMENT_PLAYING ||
        m_eExperimentState ==
          Webviz::EExperimentState::EXPERIMENT_FAST_FORWARDING) {
        if (!m_cSimulator.IsExperimentFinished()) {
          /* Run user's pre step function */
          m_cSimulator.GetLoopFunctions().PreStep();

          if (m_bFastForwarding) {
            /* Number of frames to drop in fast-forward */
            unFFStepCounter = m_unDrawFrameEvery;
          } else {
            /* For non-fastforwarding mode, steps is 1 */
            unFFStepCounter = 1;
          }

          /* Loop for steps (multiple for fast-forward) */
          while (unFFStepCounter > 0 && !m_cSimulator.IsExperimentFinished() &&
                 (m_eExperimentState ==
                    Webviz::EExperimentState::EXPERIMENT_PLAYING ||
                  m_eExperimentState ==
                    Webviz::EExperimentState::EXPERIMENT_FAST_FORWARDING)) {
            /* Run one step */
            m_cSimulator.UpdateSpace();

            /* Steps counter in this while loop */
            --unFFStepCounter;
          }

          /* Broadcast current experiment state */
          BroadcastExperimentState();

          /* Run user's post step function */
          m_cSimulator.GetLoopFunctions().PostStep();

          /* Experiment done while in while loop */
          if (m_cSimulator.IsExperimentFinished()) {
            /* The experiment is done */
            m_cSimulator.GetLoopFunctions().PostExperiment();

            ResetExperiment();

            /* Change state and emit signals */
            m_cWebServer->EmitEvent("Experiment done", m_eExperimentState);
            LOG << "[INFO] Experiment done\n";
            return; /* Go back once done */
          }

          /* Take the time now */
          m_cTimer.Stop();

          /* If the elapsed time is lower than the tick length, wait */
          if (m_cTimer.Elapsed() < m_cSimulatorTickMillis) {
            /* Sleep for the difference duration */
            std::this_thread::sleep_for(
              m_cSimulatorTickMillis - m_cTimer.Elapsed());
          } else {
            LOG << "[WARNING] Clock tick took " << m_cTimer
                << " milli-secs, more than the expected "
                << m_cSimulatorTickMillis.count() << " milli-secs. "
                << "Recovering in next cycle." << std::endl;
          }

          /* Restart Timer */
          m_cTimer.Start();
        } else {
          /* The experiment is already done */
          m_cSimulator.GetLoopFunctions().PostExperiment();

          ResetExperiment();

          /* Change state and emit signals */
          m_cWebServer->EmitEvent("Experiment done", m_eExperimentState);
          LOG << "[INFO] Experiment done\n";
        }
      } else {
        /*
         * Update the experiment state variable and sleep for some time,
         * we sleep to reduce the number of updates done in
         * "PAUSED"/"INITIALIZED" state
         */
        BroadcastExperimentState();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }
  }

  /****************************************/
  /****************************************/

  void CWebviz::PlayExperiment() {
    /* Make sure we are in the right state */
    if (
      m_eExperimentState != Webviz::EExperimentState::EXPERIMENT_INITIALIZED &&
      m_eExperimentState != Webviz::EExperimentState::EXPERIMENT_PAUSED) {
      LOG << "[WARNING] CWebviz::PlayExperiment() called in wrong state: "
          << Webviz::EExperimentStateToStr(m_eExperimentState) << std::endl;

      // silently return;
      return;
    }
    /* Disable fast-forward */
    m_bFastForwarding = false;

    m_cSimulatorTickMillis = std::chrono::milliseconds(
      (long int)(CPhysicsEngine::GetSimulationClockTick() * 1000.0f));

    /* Change state and emit signals */
    m_eExperimentState = Webviz::EExperimentState::EXPERIMENT_PLAYING;
    m_cWebServer->EmitEvent("Experiment playing", m_eExperimentState);

    LOG << "[INFO] Experiment playing";

    m_cTimer.Start();
  }

  /****************************************/
  /****************************************/

  void CWebviz::FastForwardExperiment() {
    /* Make sure we are in the right state */
    if (
      m_eExperimentState != Webviz::EExperimentState::EXPERIMENT_INITIALIZED &&
      m_eExperimentState != Webviz::EExperimentState::EXPERIMENT_PAUSED) {
      LOG << "[WARNING] CWebviz::FastForwardExperiment() called in wrong state:"
          << Webviz::EExperimentStateToStr(m_eExperimentState)
          << "\nRunning the experiment in FastForward mode" << std::endl;
    }
    m_bFastForwarding = true;

    m_cSimulatorTickMillis = std::chrono::milliseconds(
      (long int)(CPhysicsEngine::GetSimulationClockTick() * 1000.0f));

    /* Change state and emit signals */
    m_eExperimentState = Webviz::EExperimentState::EXPERIMENT_FAST_FORWARDING;
    m_cWebServer->EmitEvent("Experiment fast-forwarding", m_eExperimentState);

    LOG << "[INFO] Experiment fast-forwarding";

    m_cTimer.Start();
  }

  /****************************************/
  /****************************************/

  void CWebviz::PauseExperiment() {
    /* Make sure we are in the right state */
    if (
      m_eExperimentState != Webviz::EExperimentState::EXPERIMENT_PLAYING &&
      m_eExperimentState !=
        Webviz::EExperimentState::EXPERIMENT_FAST_FORWARDING) {
      LOG << "[WARNING] CWebviz::PauseExperiment() called in wrong state: "
          << Webviz::EExperimentStateToStr(m_eExperimentState);
      throw std::runtime_error(
        "Cannot pause the experiment, current state : " +
        Webviz::EExperimentStateToStr(m_eExperimentState));
      return;
    }
    /* Disable fast-forward */
    m_bFastForwarding = false;

    /* Change state and emit signals */
    m_eExperimentState = Webviz::EExperimentState::EXPERIMENT_PAUSED;
    m_cWebServer->EmitEvent("Experiment paused", m_eExperimentState);

    LOG << "[INFO] Experiment paused";
  }

  /****************************************/
  /****************************************/

  void CWebviz::StepExperiment() {
    /* Make sure we are in the right state */
    if (
      m_eExperimentState == Webviz::EExperimentState::EXPERIMENT_PLAYING ||
      m_eExperimentState ==
        Webviz::EExperimentState::EXPERIMENT_FAST_FORWARDING) {
      LOG << "[WARNING] CWebviz::StepExperiment() called in wrong state: "
          << Webviz::EExperimentStateToStr(m_eExperimentState)
          << " pausing the experiment to run a step";

      /* Make experiment pause */
      m_eExperimentState = Webviz::EExperimentState::EXPERIMENT_PAUSED;

      /* Do not go further, as the while loop in SimulationThreadFunction might
       * be halfway into execution */
      return;
    }

    /* Disable fast-forward */
    m_bFastForwarding = false;

    if (!m_cSimulator.IsExperimentFinished()) {
      /* Run user's pre step function */
      m_cSimulator.GetLoopFunctions().PreStep();

      /* Run one step */
      m_cSimulator.UpdateSpace();

      /* Run user's post step function */
      m_cSimulator.GetLoopFunctions().PostStep();

      /* Change state and emit signals */
      m_cWebServer->EmitEvent("Experiment step done", m_eExperimentState);
    } else {
      /* The experiment is done */
      m_cSimulator.GetLoopFunctions().PostExperiment();

      ResetExperiment();

      /* Change state and emit signals */
      m_cWebServer->EmitEvent("Experiment done", m_eExperimentState);
      LOG << "[INFO] Experiment done\n";
    }

    /* Broadcast current experiment state */
    BroadcastExperimentState();
  }

  /****************************************/
  /****************************************/

  void CWebviz::ResetExperiment() {
    /* Reset Simulator */
    m_cSimulator.Reset();

    /* Disable fast-forward */
    m_bFastForwarding = false;

    m_eExperimentState = Webviz::EExperimentState::EXPERIMENT_INITIALIZED;

    /* Change state and emit signals */
    m_cWebServer->EmitEvent("Experiment reset", m_eExperimentState);

    /* Broadcast current experiment state */
    BroadcastExperimentState();

    LOG << "[INFO] Experiment reset";
  }

  /****************************************/
  /****************************************/

  void CWebviz::BroadcastExperimentState() {
    nlohmann::json cStateJson;

    /* Get all entities in the experiment */
    CEntity::TVector& vecEntities = m_cSpace.GetRootEntityVector();
    for (CEntity::TVector::iterator itEntities = vecEntities.begin();
         itEntities != vecEntities.end();
         ++itEntities) {
      auto cEntityJSON = CallEntityOperation<
        CWebvizOperationGenerateJSON,
        CWebviz,
        nlohmann::json>(*this, **itEntities);
      if (cEntityJSON != nullptr) {
        cStateJson["entities"].push_back(cEntityJSON);
      } else {
        LOGERR << "[ERROR] Entity cannot be converted:";
        LOGERR << (**itEntities).GetTypeDescription();
      }
    }

    const CVector3& cArenaSize = m_cSpace.GetArenaSize();
    cStateJson["arena"]["size"]["x"] = cArenaSize.GetX();
    cStateJson["arena"]["size"]["y"] = cArenaSize.GetY();
    cStateJson["arena"]["size"]["z"] = cArenaSize.GetZ();

    const CVector3& cArenaCenter = m_cSpace.GetArenaCenter();
    cStateJson["arena"]["center"]["x"] = cArenaCenter.GetX();
    cStateJson["arena"]["center"]["y"] = cArenaCenter.GetY();
    cStateJson["arena"]["center"]["z"] = cArenaCenter.GetZ();

    // m_cSpace.GetArenaLimits();

    /* Added Unix Epoch in milliseconds */
    cStateJson["timestamp"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();

    /* Current state of the experiment */
    cStateJson["state"] = Webviz::EExperimentStateToStr(m_eExperimentState);

    /* Number of step from the simulator */
    cStateJson["steps"] = m_cSpace.GetSimulationClock();

    /* Send to webserver to broadcast */
    m_cWebServer->Broadcast(cStateJson);
  }

  /****************************************/
  /****************************************/

  CWebviz::~CWebviz() {
    delete m_cWebServer;
    delete m_pcLogStream;
    delete m_pcLogErrStream;
  }

  /****************************************/
  /****************************************/

  void CWebviz::Reset() {}

  /****************************************/
  /****************************************/

  void CWebviz::Destroy() {}
  /****************************************/
  /****************************************/

  REGISTER_VISUALIZATION(
    CWebviz,
    "webviz",
    "Prajankya [contact@prajankya.me]",
    "1.0",
    "WebViz to render over web in clientside.",
    " -- .\n",
    "It allows the user to watch and modify the "
    "simulation as it's running in an\n"
    "intuitive way.\n\n"
    "REQUIRED XML CONFIGURATION\n\n"
    "  <visualization>\n"
    "    <webviz />\n"
    "  </visualization>\n\n"
    "OPTIONAL XML CONFIGURATION\n\n");
}  // namespace argos