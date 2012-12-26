/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * SLPServerSATest.cpp
 * Tests the SA functionality of the SLPServer class
 * Copyright (C) 2012 Simon Newton
 */

#include <stdint.h>
#include <cppunit/extensions/HelperMacros.h>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include "ola/Clock.h"
#include "ola/Logging.h"
#include "ola/io/SelectServer.h"
#include "ola/network/IPV4Address.h"
#include "ola/network/SocketAddress.h"
#include "ola/testing/MockUDPSocket.h"
#include "ola/testing/TestUtils.h"
#include "tools/slp/SLPPacketBuilder.h"
#include "tools/slp/SLPPacketConstants.h"
#include "tools/slp/SLPServer.h"
#include "tools/slp/ScopeSet.h"
#include "tools/slp/ServiceEntry.h"
#include "tools/slp/URLEntry.h"

using ola::io::SelectServer;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::slp::PARSE_ERROR;
using ola::slp::SCOPE_NOT_SUPPORTED;
using ola::slp::SLPPacketBuilder;
using ola::slp::SLPServer;
using ola::slp::SLP_OK;
using ola::slp::ScopeSet;
using ola::slp::ServiceEntry;
using ola::slp::URLEntries;
using ola::slp::URLEntry;
using ola::slp::xid_t;
using ola::testing::MockUDPSocket;
using ola::testing::SocketVerifier;
using std::auto_ptr;
using std::set;
using std::string;


class SLPServerSATest: public CppUnit::TestFixture {
  public:
    SLPServerSATest()
        : m_ss(NULL, &m_clock) {
    }

  public:
    CPPUNIT_TEST_SUITE(SLPServerSATest);
    CPPUNIT_TEST(testSrvRqst);
    CPPUNIT_TEST(testSrvRqstForServiceAgent);
    CPPUNIT_TEST(testMissingServiceType);
    CPPUNIT_TEST(testMisconfiguredSA);
    CPPUNIT_TEST_SUITE_END();

    void testSrvRqst();
    void testSrvRqstForServiceAgent();
    void testMissingServiceType();
    void testMisconfiguredSA();

  public:
    void setUp() {
      ola::InitLogging(ola::OLA_LOG_INFO, ola::OLA_LOG_STDERR);
      m_udp_socket.Init();
      m_udp_socket.SetInterface(IPV4Address::FromStringOrDie(SERVER_IP));
      m_udp_socket.Bind(IPV4SocketAddress(IPV4Address::WildCard(),
                        SLP_TEST_PORT));
      // make sure WakeUpTime is populated
      m_ss.RunOnce(0, 0);
    }

    // Advance the time, which may trigger timeouts to run
    void AdvanceTime(int32_t sec, int32_t usec) {
      m_clock.AdvanceTime(sec, usec);
      // run any timeouts, and update the WakeUpTime
      m_ss.RunOnce(0, 0);
    }

  private:
    typedef set<IPV4Address> PRList;

    ola::MockClock m_clock;
    ola::io::SelectServer m_ss;
    MockUDPSocket m_udp_socket;
    auto_ptr<SLPServer> m_server;

    SLPServer *CreateNewServer(bool enable_da, const string &scopes);
    void InjectServiceRequest(const IPV4SocketAddress &source,
                              xid_t xid,
                              bool multicast,
                              const set<IPV4Address> &pr_list,
                              const string &service_type,
                              const ScopeSet &scopes);

    void ExpectServiceReply(const IPV4SocketAddress &dest,
                            xid_t xid,
                            uint16_t error_code,
                            const URLEntries &urls);
    void ExpectSAAdvert(const IPV4SocketAddress &dest,
                        xid_t xid,
                        bool multicast,
                        const string &url,
                        const ScopeSet &scopes);

    static const uint16_t SLP_TEST_PORT = 5570;
    static const char SERVER_IP[];
};


const char SLPServerSATest::SERVER_IP[] = "10.0.0.1";

CPPUNIT_TEST_SUITE_REGISTRATION(SLPServerSATest);


SLPServer *SLPServerSATest::CreateNewServer(bool enable_da,
                                            const string &scopes) {
  ScopeSet scope_set(scopes);

  SLPServer::SLPServerOptions options;
  options.enable_da = enable_da;
  options.clock = &m_clock;
  options.ip_address = IPV4Address::FromStringOrDie(SERVER_IP);
  options.initial_xid = 0;  // don't randomize the xid for testing
  options.scopes = set<string>();
  copy(scope_set.begin(), scope_set.end(),
       std::inserter(options.scopes, options.scopes.end()));
  options.slp_port = 5570;
  SLPServer *server = new SLPServer(&m_ss, &m_udp_socket, NULL, NULL, options);
  // TODO(simon): test without the Init here
  server->Init();
  return server;
}


/**
 * Inject a SrvRqst into the UDP socket.
 */
void SLPServerSATest::InjectServiceRequest(const IPV4SocketAddress &source,
                                           xid_t xid,
                                           bool multicast,
                                           const set<IPV4Address> &pr_list,
                                           const string &service_type,
                                           const ScopeSet &scopes) {
  ola::io::IOQueue output;
  ola::io::BigEndianOutputStream output_stream(&output);
  SLPPacketBuilder::BuildServiceRequest(
      &output_stream, xid, multicast, pr_list, service_type, scopes);
  m_udp_socket.InjectData(&output, source);
}


/**
 * Expect a SrvRepl
 */
void SLPServerSATest::ExpectServiceReply(const IPV4SocketAddress &dest,
                                         xid_t xid,
                                         uint16_t error_code,
                                         const URLEntries &urls) {
  ola::io::IOQueue output;
  ola::io::BigEndianOutputStream output_stream(&output);
  SLPPacketBuilder::BuildServiceReply(&output_stream, xid, error_code, urls);
  m_udp_socket.AddExpectedData(&output, dest);
  OLA_ASSERT_TRUE(output.Empty());
}


/**
 * Expect a SAAdvert
 */
void SLPServerSATest::ExpectSAAdvert(const IPV4SocketAddress &dest,
                                     xid_t xid,
                                     bool multicast,
                                     const string &url,
                                     const ScopeSet &scopes) {
  ola::io::IOQueue output;
  ola::io::BigEndianOutputStream output_stream(&output);
  SLPPacketBuilder::BuildSAAdvert(&output_stream, xid, multicast, url, scopes);
  m_udp_socket.AddExpectedData(&output, dest);
  OLA_ASSERT_TRUE(output.Empty());
}


/**
 * Test the SA when no DAs are present.
 */
void SLPServerSATest::testSrvRqst() {
  m_server.reset(CreateNewServer(false, "one"));

  // register a service with the instance
  ServiceEntry service("one,two", "service:foo://localhost", 300);
  OLA_ASSERT_EQ((uint16_t) SLP_OK, m_server->RegisterService(service));
  AdvanceTime(0, 0);

  IPV4SocketAddress peer = IPV4SocketAddress::FromStringOrDie(
      "192.168.1.1:5570");
  xid_t xid = 10;

  // send a multicast SrvRqst, expect a SrvRply
  {
    SocketVerifier verifier(&m_udp_socket);

    URLEntries urls;
    urls.push_back(service.url());
    ExpectServiceReply(peer, xid, SLP_OK, urls);

    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, xid, true, pr_list, "service:foo", scopes);
  }

  // send a unicast SrvRqst, expect a SrvRply
  {
    SocketVerifier verifier(&m_udp_socket);

    URLEntries urls;
    urls.push_back(service.url());
    ExpectServiceReply(peer, ++xid, SLP_OK, urls);

    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:foo", scopes);
  }

  // Try a multicast request but with the SA's IP in the PR list
  {
    SocketVerifier verifier(&m_udp_socket);
    ScopeSet scopes("one");
    PRList pr_list;
    pr_list.insert(IPV4Address::FromStringOrDie(SERVER_IP));
    InjectServiceRequest(peer, ++xid, true, pr_list, "service:foo", scopes);
  }

  // test a multicast request for a scope that doesn't match the SAs scopes
  {
    SocketVerifier verifier(&m_udp_socket);
    ScopeSet scopes("two");
    PRList pr_list;
    InjectServiceRequest(peer, ++xid, true, pr_list, "service:foo", scopes);
  }

  // test a unicast request for a scope that doesn't match the SAs scopes
  {
    SocketVerifier verifier(&m_udp_socket);
    URLEntries urls;
    ExpectServiceReply(peer, ++xid, SCOPE_NOT_SUPPORTED, urls);

    ScopeSet scopes("two");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:foo", scopes);
  }

  // test a multicast request with no scope list
  {
    SocketVerifier verifier(&m_udp_socket);
    ScopeSet scopes("");
    PRList pr_list;
    InjectServiceRequest(peer, ++xid, true, pr_list, "service:foo", scopes);
  }

  // test a unicast request with no scope list
  {
    SocketVerifier verifier(&m_udp_socket);
    URLEntries urls;
    ExpectServiceReply(peer, ++xid, SCOPE_NOT_SUPPORTED, urls);

    ScopeSet scopes("");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:foo", scopes);
  }

  // de-register, then we should receive no response to a multicast request
  {
    SocketVerifier verifier(&m_udp_socket);
    OLA_ASSERT_EQ((uint16_t) SLP_OK, m_server->DeRegisterService(service));
    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, ++xid, true, pr_list, "service:foo", scopes);
  }

  // a unicast request should return a SrvRply with no URL entries
  {
    SocketVerifier verifier(&m_udp_socket);
    URLEntries urls;
    ExpectServiceReply(peer, ++xid, SLP_OK, urls);

    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:foo", scopes);
  }
}


/**
 * Test for SrvRqsts of the form service:service-agent
 */
void SLPServerSATest::testSrvRqstForServiceAgent() {
  m_server.reset(CreateNewServer(false, "one,two"));

  IPV4SocketAddress peer = IPV4SocketAddress::FromStringOrDie(
      "192.168.1.1:5570");
  xid_t xid = 10;

  // send a unicast SrvRqst, expect a SAAdvert
  {
    SocketVerifier verifier(&m_udp_socket);
    ExpectSAAdvert(peer, xid, false, "service:service-agent://10.0.0.1",
                   ScopeSet("one,two"));

    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:service-agent",
                         scopes);
  }

  // send a multicast SrvRqst, expect a SAAdvert
  {
    SocketVerifier verifier(&m_udp_socket);
    ExpectSAAdvert(peer, xid, false, "service:service-agent://10.0.0.1",
                   ScopeSet("one,two"));

    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, xid, true, pr_list, "service:service-agent",
                         scopes);
  }

  // send a unicast SrvRqst with no scopes, this should generate a response
  {
    SocketVerifier verifier(&m_udp_socket);
    ExpectSAAdvert(peer, xid, false, "service:service-agent://10.0.0.1",
                   ScopeSet("one,two"));

    ScopeSet scopes;
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:service-agent",
                         scopes);
  }

  // send a multicast SrvRqst with no scopes, this should generate a response
  {
    SocketVerifier verifier(&m_udp_socket);
    ExpectSAAdvert(peer, xid, false, "service:service-agent://10.0.0.1",
                   ScopeSet("one,two"));

    ScopeSet scopes;
    PRList pr_list;
    InjectServiceRequest(peer, xid, true, pr_list, "service:service-agent",
                         scopes);
  }

  // send a unicast SrvRqst with scopes that don't match, expect an error
  {
    SocketVerifier verifier(&m_udp_socket);
    URLEntries urls;
    ExpectServiceReply(peer, ++xid, SCOPE_NOT_SUPPORTED, urls);
    ScopeSet scopes("three");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:service-agent",
                         scopes);
  }

  // send a multicast SrvRqst with scopes that don't match, no response is
  // expected.
  {
    SocketVerifier verifier(&m_udp_socket);
    ScopeSet scopes("three");
    PRList pr_list;
    InjectServiceRequest(peer, xid, true, pr_list, "service:service-agent",
                         scopes);
  }
}


/**
 * Test for a missing service type
 */
void SLPServerSATest::testMissingServiceType() {
  m_server.reset(CreateNewServer(false, "one"));

  IPV4SocketAddress peer = IPV4SocketAddress::FromStringOrDie(
      "192.168.1.1:5570");
  xid_t xid = 10;

  // send a unicast SrvRqst, expect a SAAdvert
  {
    SocketVerifier verifier(&m_udp_socket);
    URLEntries urls;
    ExpectServiceReply(peer, ++xid, PARSE_ERROR, urls);

    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "", scopes);
  }

  // send a multicast SrvRqst, this is silently dropped
  {
    SocketVerifier verifier(&m_udp_socket);
    ScopeSet scopes("one");
    PRList pr_list;
    InjectServiceRequest(peer, xid, true, pr_list, "", scopes);
  }
}


/**
 * Test that we can't configure an SA with no scopes.
 */
void SLPServerSATest::testMisconfiguredSA() {
  // this should switch to 'default'
  m_server.reset(CreateNewServer(false, ""));

  IPV4SocketAddress peer = IPV4SocketAddress::FromStringOrDie(
      "192.168.1.1:5570");
  xid_t xid = 10;

  // send a unicast SrvRqst, expect a SAAdvert
  {
    SocketVerifier verifier(&m_udp_socket);
    ExpectSAAdvert(peer, xid, false, "service:service-agent://10.0.0.1",
                   ScopeSet("default"));

    ScopeSet scopes("");
    PRList pr_list;
    InjectServiceRequest(peer, xid, false, pr_list, "service:service-agent",
                         scopes);
  }
}
