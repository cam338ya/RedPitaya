#include <vector>
#include "dac_streaming.h"

CDACStreamingApplication *g_dac_app = nullptr;
CDACStreamingManager::Ptr g_dac_manger = nullptr;
bool					  g_dac_verbMode = false;
CGenerator::Ptr 		  gen = nullptr;

std::shared_ptr<ServerNetConfigManager> g_serverDACNetConfig = nullptr;
std::atomic_bool g_dac_serverRun(false);

auto calibDACFullScaleToVoltage(uint32_t fullScaleGain) -> float {
    /* no scale */
    if (fullScaleGain == 0) {
        return 1;
    }
    return (float) ((float)fullScaleGain  * 100.0 / ((uint64_t)1<<32));
}

auto startDACServer(std::shared_ptr<ServerNetConfigManager> serverNetConfig,bool verbMode,bool testMode) -> void{
    if (!serverNetConfig) return;
    g_serverDACNetConfig = serverNetConfig;
	gen = nullptr;
	g_dac_verbMode = verbMode;
	try{
		CStreamSettings settings = testMode ? g_serverDACNetConfig->getTempSettings() : g_serverDACNetConfig->getSettings();
		if (!settings.isSetted()) return;

		g_dac_serverRun = true;
		auto use_file     =  settings.getDACMode();
		auto sock_port    =  settings.getDACPort();
		auto dac_speed    =  settings.getDACHz();
		auto ip_addr_host = "127.0.0.1";

#ifdef Z20
		auto use_calib    = 0;
		auto attenuator   = 0;
#else
		auto use_calib    = settings.getCalibration();
		rp_CalibInit();
		auto osc_calib_params = rp_GetCalibrationSettings();
#endif

#ifdef Z20_250_12
		auto dac_gain = settings.getDACGain();
#endif

		std::vector<UioT> uioList = GetUioList();
		uint32_t ch1_off = 0;
		uint32_t ch2_off = 0;
		float ch1_gain = 1;
		float ch2_gain = 1;

		if (use_calib == 1) {
#ifdef Z20_250_12
			if (dac_gain == CStreamSettings::X1) {			
				ch1_gain = calibDACFullScaleToVoltage(osc_calib_params.gen_ch1_g_1);  
				ch2_gain = calibDACFullScaleToVoltage(osc_calib_params.gen_ch2_g_1);  
				ch1_off  = osc_calib_params.gen_ch1_off_1;
				ch2_off  = osc_calib_params.gen_ch2_off_1;
			}else{
				ch1_gain = calibDACFullScaleToVoltage(osc_calib_params.gen_ch1_g_5);  
				ch2_gain = calibDACFullScaleToVoltage(osc_calib_params.gen_ch2_g_5);  
				ch1_off  = osc_calib_params.gen_ch1_off_5;
				ch2_off  = osc_calib_params.gen_ch2_off_5;
			}
#endif

#if defined Z10 || defined Z20_125
			ch1_gain = calibDACFullScaleToVoltage(osc_calib_params.be_ch1_fs);
			ch2_gain = calibDACFullScaleToVoltage(osc_calib_params.be_ch2_fs);
			ch1_off  = osc_calib_params.be_ch1_dc_offs;
			ch2_off  = osc_calib_params.be_ch2_dc_offs;
#endif
		}

#ifdef Z20_250_12

        rp_max7311::rp_setGainOut(RP_MAX7311_OUT1, dac_gain == CStreamSettings::X1 ? RP_GAIN_2V : RP_GAIN_10V);
        rp_max7311::rp_setGainOut(RP_MAX7311_OUT2, dac_gain == CStreamSettings::X1 ? RP_GAIN_2V : RP_GAIN_10V);

#endif

		if (g_dac_app!= nullptr){
			g_dac_app->stop();
			delete g_dac_app;
		}

		for (const UioT &uio : uioList)
		{
			 if (uio.nodeName == "rp_dac")
			{
				gen = CGenerator::Create(uio, true , true ,dac_speed,DAC_FREQUENCY);
				gen->setCalibration(ch1_off,ch1_gain,ch2_off,ch2_gain);
				gen->setDacHz(dac_speed);
			}
		}

		if (!gen){
			fprintf(stderr,"[Streaming] Error init generator module\n");
        	syslog (LOG_ERR, "[Streaming] Error init generator module\n");
			return;
		}


		if (use_file == CStreamSettings::DAC_NET) {
			g_dac_manger = CDACStreamingManager::Create(
					ip_addr_host,
					sock_port);
		}else{

			auto format = settings.getDACFileType();
			auto filePath = settings.getDACFile();
			auto dacRepeatMode = settings.getDACRepeat();
			auto dacRepeatCount = settings.getDACRepeatCount();
			auto dacMemory = settings.getDACMemoryUsage();
			
			if (format == CStreamSettings::WAV) {
				g_dac_manger = CDACStreamingManager::Create(CDACStreamingManager::WAV_TYPE,filePath,dacRepeatMode,dacRepeatCount,dacMemory);
			}else if (format == CStreamSettings::TDMS) {
				g_dac_manger = CDACStreamingManager::Create(CDACStreamingManager::TDMS_TYPE,filePath,dacRepeatMode,dacRepeatCount,dacMemory);
			}else{
				return;
			}
			g_dac_manger->notifyStop = [](CDACStreamingManager::NotifyResult status)
			{
				stopDACNonBlocking(status);
			};
		
		}

		g_dac_app = new CDACStreamingApplication(g_dac_manger, gen);
		g_dac_app->setVerbousMode(g_dac_verbMode);
		g_dac_app->setTestMode(testMode);

		g_dac_app->runNonBlock();
		if (g_dac_manger->isLocalMode()){
			g_serverDACNetConfig->sendDACServerStartedSD();
		}else{
			g_serverDACNetConfig->sendDACServerStarted();
		}
		if (g_dac_verbMode){
			fprintf(stdout,"[Streaming] Start dac server\n");
        	syslog (LOG_NOTICE, "[Streaming] Start dac server\n");
		}
	}catch (std::exception& e)
	{
		fprintf(stderr, "Error: startDACServer() %s\n",e.what());
        syslog (LOG_ERR,"Error: startDACServer() %s\n",e.what());
	}
}

auto stopDACNonBlocking(CDACStreamingManager::NotifyResult x) -> void{
	try{
		std::thread th(stopDACServer ,x);
		th.detach();
	}catch (std::exception& e)
	{
		fprintf(stderr, "Error: StopNonBlocking() %s\n",e.what());
        syslog (LOG_ERR,"Error: StopNonBlocking() %s\n",e.what());
	}
}


auto stopDACServer(CDACStreamingManager::NotifyResult x) -> void{
	try{
		if (g_dac_app)
		{
			g_dac_app->stop();
			delete g_dac_app;
			g_dac_app = nullptr;
		}
        if (g_serverDACNetConfig){
            switch (x)
            {
				case CDACStreamingManager::NR_STOP:
					g_serverDACNetConfig->sendDACServerStopped();
					break;
				case CDACStreamingManager::NR_ENDED:
					g_serverDACNetConfig->sendDACServerStoppedSDDone();
					break;
				case CDACStreamingManager::NR_EMPTY:
					g_serverDACNetConfig->sendDACServerStoppedSDEmpty();
					break;
				case CDACStreamingManager::NR_BROKEN:
					g_serverDACNetConfig->sendDACServerStoppedSDBroken();
					break;
				case CDACStreamingManager::NR_MISSING_FILE:
					g_serverDACNetConfig->sendDACServerStoppedSDMissingFile();
					break;
				default:
					throw runtime_error("Unknown state");
					break;
            }
        }
		g_dac_serverRun = false;
		if (g_dac_verbMode){
        	fprintf(stdout,"[Streaming] Stop dac server\n");
        	syslog (LOG_NOTICE, "[Streaming] Stop dac server\n");
		}
	}catch (std::exception& e)
	{
		fprintf(stderr, "Error: stopDACServer() %s\n",e.what());
        syslog (LOG_ERR,"Error: stopDACServer() %s\n",e.what());
	}
}