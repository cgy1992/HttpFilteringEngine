/*
* Copyright � 2017 Jesse Nicholson
* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include "HttpFilteringEngineControl.hpp"

#include <boost/predef.h>

// Because we're using ::asio inside a library, and it's a header only library,
// we need to include the source in one place alone, and that's here. We also
// need to add some preprocessor directives. This is all covered in the docs
// here: http://www.boost.org/doc/libs/1_60_0/doc/html/boost_asio/using.html#boost_asio.using.optional_separate_compilation
#include <boost/asio/impl/src.hpp>
#include <boost/asio/ssl/impl/src.hpp>

// On Windows, because of how WinSock requires manual startup/shutdown on a 
// per-process basis, boost::asio uses an atomic static reference counting
// system to transparently handle this tedious process. Since we're consuming
// boost::asio into a shared lib rather than a exe, this system does not 
// function correctly. This is the solution according to the docs, to manually
// force that static ref count to increment, which will cause it to never
// decrement below 1, and therefore will not call WSACleanup on us.
//
// As such, it's up to us to manually call WSAStartup/WSACleanup.
#include <boost/predef/os.h>
#include <boost/predef/compiler.h>

#if BOOST_OS_WINDOWS

	#include <boost/asio/detail/winsock_init.hpp>

	#ifdef _MSC_VER
		#pragma warning(push)
		#pragma warning(disable:4073)
		#pragma init_seg(lib)
	#endif

	boost::asio::detail::winsock_init<>::manual manual_winsock_init;

	#ifdef _MSC_VER
		#pragma warning(pop)
	#endif

	#include "mitm/secure/WindowsInMemoryCertificateStore.hpp"
#else
	#include "NO_PLATFORM_SPECIFIC_CERTIFICATE_STORE_FOUND.hpp"
#endif

#include "filtering/http/HttpFilteringEngine.hpp"
#include "mitm/diversion/DiversionControl.hpp"
#include "filtering/options/ProgramWideOptions.hpp"

namespace te
{
	namespace httpengine
	{

		HttpFilteringEngineControl::HttpFilteringEngineControl(
			util::cb::FirewallCheckFunction firewallCb,
			std::string caBundleAbsolutePath,
			std::string blockedHtmlPage,
			uint16_t httpListenerPort,
			uint16_t httpsListenerPort,
			uint32_t proxyNumThreads,
			util::cb::ContentClassificationFunction onClassify,
			util::cb::MessageFunction onInfo,
			util::cb::MessageFunction onWarn,
			util::cb::MessageFunction onError,
			util::cb::RequestBlockFunction onRequestBlocked,
			util::cb::ElementBlockFunction onElementsBlocked
			)
			:
			util::cb::EventReporter(onInfo, onWarn, onError),			
			m_firewallCheckCb(firewallCb),
			m_caBundleAbsolutePath(caBundleAbsolutePath),
			m_httpListenerPort(httpListenerPort),
			m_httpsListenerPort(httpsListenerPort),
			m_proxyNumThreads(proxyNumThreads),
			m_programWideOptions(new filtering::options::ProgramWideOptions(blockedHtmlPage)),
			m_httpFilteringEngine(new filtering::http::HttpFilteringEngine(m_programWideOptions.get(), onInfo, onWarn, onError, onClassify, onRequestBlocked, onElementsBlocked)),
			m_isRunning(false)
		{
			if (m_store == nullptr)
			{
				// XXX TODO - Make a factory for cert store so we don't have this horrible mess everywhere.
				#if BOOST_OS_WINDOWS
					m_store.reset(new mitm::secure::WindowsInMemoryCertificateStore(u8"CA", u8"Http Filtering Engine", u8"Http Filtering Engine"));
				#elif BOOST_OS_ANDROID
					You poor guy.You didn't write a cert store for Android. Are you new?
				#else
					You poor guy.You didn't write a cert store for this OS. Are you new ?
				#endif

				if (!m_store->EstablishOsTrust())
				{
					throw std::runtime_error(u8"In HttpFilteringEngineControl::Start() - Failed to establish certificate trust with OS.");
				}
			}
		}

		HttpFilteringEngineControl::~HttpFilteringEngineControl()
		{
			// Cleanup any installed certs HERE.
			if (m_store != nullptr)
			{
				try
				{
					m_store->RevokeOsTrust();
				}
				catch (std::runtime_error& e)
				{
					// XXX TODO What can we really do here?
				}				
			}
		}

		void HttpFilteringEngineControl::Start()
		{
			std::lock_guard<std::mutex> lock(m_ctlMutex);

			if (m_isRunning == false)
			{
				if (m_service == nullptr)
				{
					m_service.reset(new boost::asio::io_service());
				}
				else					
				{					
					m_service->reset();
				}				

				m_httpAcceptor.reset(
					new mitm::secure::TcpAcceptor(
						m_service.get(),
						m_httpFilteringEngine.get(),
						m_httpListenerPort,
						m_caBundleAbsolutePath,
						nullptr,
						m_onInfo,
						m_onWarning,
						m_onError
						)
					);

				#ifdef NDEBUG
					assert(m_store != nullptr && "In HttpFilteringEngineControl::Start() - Cert store is nullptr!");
				#endif

				m_httpsAcceptor.reset(
					new mitm::secure::TlsAcceptor(
						m_service.get(),
						m_httpFilteringEngine.get(),
						m_httpsListenerPort,
						m_caBundleAbsolutePath,
						m_store.get(),
						m_onInfo,
						m_onWarning,
						m_onError
						)
					);

				m_httpAcceptor->AcceptConnections();

				m_httpsAcceptor->AcceptConnections();

				m_diversionControl.reset(new mitm::diversion::DiversionControl(m_firewallCheckCb, m_onInfo, m_onWarning, m_onError));

				m_diversionControl->SetHttpListenerPort(m_httpAcceptor->GetListenerPort());

				m_diversionControl->SetHttpsListenerPort(m_httpsAcceptor->GetListenerPort());

				m_diversionControl->Run();

				for (uint32_t i = 0; i < m_proxyNumThreads; ++i)
				{
					m_proxyServiceThreads.emplace_back(
						std::thread
							{ 
								std::bind(
								static_cast<size_t(boost::asio::io_service::*)()>(&boost::asio::io_service::run), 
									std::ref(*m_service.get())
									) 
							}
					);
				}

				m_isRunning = true;
			}			
		}

		void HttpFilteringEngineControl::Stop()
		{
			std::lock_guard<std::mutex> lock(m_ctlMutex);
			
			if (m_isRunning == true)
			{				
				m_httpAcceptor->StopAccepting();
				m_httpsAcceptor->StopAccepting();
				m_diversionControl->Stop();
				m_service->stop();

				for (auto& t : m_proxyServiceThreads)
				{
					t.join();
				}

				m_proxyServiceThreads.clear();

				m_isRunning = false;
			}
		}

		bool HttpFilteringEngineControl::IsRunning() const
		{
			return m_isRunning;
		}

		const uint32_t HttpFilteringEngineControl::GetHttpListenerPort() const
		{
			if (m_isRunning && m_httpAcceptor != nullptr)
			{
				return m_httpAcceptor->GetListenerPort();
			}

			return 0;
		}

		const uint32_t HttpFilteringEngineControl::GetHttpsListenerPort() const
		{
			if (m_isRunning && m_httpsAcceptor != nullptr)
			{
				return m_httpsAcceptor->GetListenerPort();
			}

			return 0;
		}		

		void HttpFilteringEngineControl::SetOptionEnabled(const uint32_t option, const bool enabled)
		{
			if (m_programWideOptions != nullptr)
			{
				if (option < static_cast<uint32_t>(filtering::options::http::HttpFilteringOption::NUMBER_OF_ENTRIES))
				{
					m_programWideOptions->SetIsHttpFilteringOptionEnabled(static_cast<filtering::options::http::HttpFilteringOption>(option), enabled);
				}				
			}
		}

		const bool HttpFilteringEngineControl::GetOptionEnabled(const uint32_t option) const
		{
			if (m_programWideOptions != nullptr)
			{
				if (option < static_cast<uint32_t>(filtering::options::http::HttpFilteringOption::NUMBER_OF_ENTRIES))
				{
					return m_programWideOptions->GetIsHttpFilteringOptionEnabled(static_cast<filtering::options::http::HttpFilteringOption>(option));
				}
			}

			return false;
		}

		void HttpFilteringEngineControl::SetCategoryEnabled(const uint8_t category, const bool enabled)
		{
			if (m_programWideOptions != nullptr)
			{
				m_programWideOptions->SetIsHttpCategoryFiltered(category, enabled);
			}
		}

		const bool HttpFilteringEngineControl::GetCategoryEnabled(const uint8_t category) const
		{
			if (m_programWideOptions != nullptr)
			{
				return m_programWideOptions->GetIsHttpCategoryFiltered(category);
			}

			return false;
		}

		void HttpFilteringEngineControl::LoadFilteringListFromFile(
			const std::string& filePath, 
			const uint8_t listCategory, 
			const bool flushExistingInCategory,
			uint32_t* rulesLoaded,
			uint32_t* rulesFailed
			)
		{
			if (m_httpFilteringEngine != nullptr)
			{
				auto result = m_httpFilteringEngine->LoadAbpFormattedListFromFile(filePath, listCategory, flushExistingInCategory);

				if (rulesLoaded)
				{
					*rulesLoaded = result.first;
				}

				if (rulesFailed)
				{
					*rulesFailed = result.second;
				}
			}
		}

		void HttpFilteringEngineControl::LoadFilteringListFromString(
			const std::string& listString, 
			const uint8_t listCategory, 
			const bool flushExistingInCategory,
			uint32_t* rulesLoaded,
			uint32_t* rulesFailed
			)
		{
			if (m_httpFilteringEngine != nullptr)
			{
				auto result = m_httpFilteringEngine->LoadAbpFormattedListFromString(listString, listCategory, flushExistingInCategory);

				if (rulesLoaded)
				{
					*rulesLoaded = result.first;
				}

				if (rulesFailed)
				{
					*rulesFailed = result.second;
				}
			}
		}

		uint32_t HttpFilteringEngineControl::LoadTextTriggersFromFile(const std::string& triggersFilePath, const uint8_t category, const bool flushExisting)
		{
			if (m_httpFilteringEngine != nullptr)
			{
				return m_httpFilteringEngine->LoadTextTriggersFromFile(triggersFilePath, category, flushExisting);
			}

			return 0;
		}

		uint32_t HttpFilteringEngineControl::LoadTextTriggersFromString(const std::string& triggers, const uint8_t category, const bool flushExisting)
		{
			if (m_httpFilteringEngine != nullptr)
			{
				return m_httpFilteringEngine->LoadTextTriggersFromString(triggers, category, flushExisting);
			}

			return 0;
		}

		std::vector<char> HttpFilteringEngineControl::GetRootCertificatePEM() const
		{
			if (m_store)
			{
				return m_store->GetRootCertificatePEM();
			}

			return{};
		}

		void HttpFilteringEngineControl::UnloadRulesForCategory(const uint8_t category)
		{
			if (m_httpFilteringEngine != nullptr && category != 0)
			{
				m_httpFilteringEngine->UnloadAllFilterRulesForCategory(category);
			}
		}

		void HttpFilteringEngineControl::UnloadTextTriggersForCategory(const uint8_t category)
		{
			if (m_httpFilteringEngine != nullptr && category != 0)
			{
				m_httpFilteringEngine->UnloadAllTextTriggersForCategory(category);
			}
		}		

	} /* namespace httpengine */
} /* namespace te */
