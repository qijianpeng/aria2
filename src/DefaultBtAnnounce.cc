/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "DefaultBtAnnounce.h"
#include "LogFactory.h"
#include "Logger.h"
#include "PeerListProcessor.h"
#include "util.h"
#include "prefs.h"
#include "DlAbortEx.h"
#include "message.h"
#include "SimpleRandomizer.h"
#include "DownloadContext.h"
#include "PieceStorage.h"
#include "BtRuntime.h"
#include "PeerStorage.h"
#include "Peer.h"
#include "Option.h"
#include "StringFormat.h"
#include "A2STR.h"
#include "Request.h"
#include "bencode.h"
#include "bittorrent_helper.h"
#include "wallclock.h"

namespace aria2 {

DefaultBtAnnounce::DefaultBtAnnounce
(const SharedHandle<DownloadContext>& downloadContext,
 const Option* option):
  _downloadContext(downloadContext),
  trackers(0),
  prevAnnounceTimer(0),
  interval(DEFAULT_ANNOUNCE_INTERVAL),
  minInterval(DEFAULT_ANNOUNCE_INTERVAL),
  _userDefinedInterval(0),
  complete(0),
  incomplete(0),
  announceList(downloadContext->getAttribute(bittorrent::BITTORRENT)[bittorrent::ANNOUNCE_LIST]),
  option(option),
  logger(LogFactory::getInstance()),
  _randomizer(SimpleRandomizer::getInstance())
{}

DefaultBtAnnounce::~DefaultBtAnnounce() {
}

bool DefaultBtAnnounce::isDefaultAnnounceReady() {
  return
    (trackers == 0 &&
     prevAnnounceTimer.
     difference(global::wallclock) >= (_userDefinedInterval==0?
                                       minInterval:_userDefinedInterval) &&
     !announceList.allTiersFailed());
}

bool DefaultBtAnnounce::isStoppedAnnounceReady() {
  return (trackers == 0 &&
          btRuntime->isHalt() &&
          announceList.countStoppedAllowedTier());
}

bool DefaultBtAnnounce::isCompletedAnnounceReady() {
  return (trackers == 0 &&
          pieceStorage->allDownloadFinished() &&
          announceList.countCompletedAllowedTier());
}

bool DefaultBtAnnounce::isAnnounceReady() {
  return
    isStoppedAnnounceReady() ||
    isCompletedAnnounceReady() ||
    isDefaultAnnounceReady();
}

static bool uriHasQuery(const std::string& uri)
{
  Request req;
  req.setUri(uri);
  return !req.getQuery().empty();
}

std::string DefaultBtAnnounce::getAnnounceUrl() {
  if(isStoppedAnnounceReady()) {
    if(!announceList.currentTierAcceptsStoppedEvent()) {
      announceList.moveToStoppedAllowedTier();
    }
    announceList.setEvent(AnnounceTier::STOPPED);
  } else if(isCompletedAnnounceReady()) {
    if(!announceList.currentTierAcceptsCompletedEvent()) {
      announceList.moveToCompletedAllowedTier();
    }
    announceList.setEvent(AnnounceTier::COMPLETED);
  } else if(isDefaultAnnounceReady()) {
    // If download completed before "started" event is sent to a tracker,
    // we change the event to something else to prevent us from
    // sending "completed" event.
    if(pieceStorage->allDownloadFinished() &&
       announceList.getEvent() == AnnounceTier::STARTED) {
      announceList.setEvent(AnnounceTier::STARTED_AFTER_COMPLETION);
    }
  } else {
    return A2STR::NIL;
  }
  unsigned int numWant = 50;
  if(!btRuntime->lessThanMinPeers() || btRuntime->isHalt()) {
    numWant = 0;
  }
  TransferStat stat = peerStorage->calculateStat();
  uint64_t left =
    pieceStorage->getTotalLength()-pieceStorage->getCompletedLength();
  std::string uri = announceList.getAnnounce();
  uri += uriHasQuery(uri) ? "&" : "?";
  uri += "info_hash=";
  uri += util::torrentPercentEncode(bittorrent::getInfoHash(_downloadContext),
                                    INFO_HASH_LENGTH);
  uri += "&peer_id=";
  uri += util::torrentPercentEncode(bittorrent::getStaticPeerId(),
                                    PEER_ID_LENGTH);
  uri += "&uploaded=";
  uri += util::uitos(stat.getSessionUploadLength());
  uri += "&downloaded=";
  uri += util::uitos(stat.getSessionDownloadLength());
  uri += "&left=";
  uri += util::uitos(left);
  uri += "&compact=1";
  uri += "&key=";
  // Use last 8 bytes of peer ID as a key
  size_t keyLen = 8;
  uri += util::torrentPercentEncode
    (bittorrent::getStaticPeerId()+PEER_ID_LENGTH-keyLen, keyLen);
  uri += "&numwant=";
  uri += util::uitos(numWant);
  uri += "&no_peer_id=1";
  if(btRuntime->getListenPort() > 0) {
    uri += "&port=";
    uri += util::uitos(btRuntime->getListenPort());
  }
  std::string event = announceList.getEventString();
  if(!event.empty()) {
    uri += "&event=";
    uri += event;
  }
  if(!trackerId.empty()) {
    uri += "&trackerid="+util::torrentPercentEncode(trackerId);
  }
  if(option->getAsBool(PREF_BT_REQUIRE_CRYPTO)) {
    uri += "&requirecrypto=1";
  } else {
    uri += "&supportcrypto=1";
  }
  if(!option->blank(PREF_BT_EXTERNAL_IP)) {
    uri += "&ip=";
    uri += option->get(PREF_BT_EXTERNAL_IP);
  }
  return uri;
}

void DefaultBtAnnounce::announceStart() {
  trackers++;
}

void DefaultBtAnnounce::announceSuccess() {
  trackers = 0;
  announceList.announceSuccess();
}

void DefaultBtAnnounce::announceFailure() {
  trackers = 0;
  announceList.announceFailure();
}

bool DefaultBtAnnounce::isAllAnnounceFailed() {
  return announceList.allTiersFailed();
}

void DefaultBtAnnounce::resetAnnounce() {
  prevAnnounceTimer = global::wallclock;
  announceList.resetTier();
}

void
DefaultBtAnnounce::processAnnounceResponse(const unsigned char* trackerResponse,
                                           size_t trackerResponseLength)
{
  if(logger->debug()) {
    logger->debug("Now processing tracker response.");
  }
  const BDE dict =
    bencode::decode(trackerResponse, trackerResponseLength);
  if(!dict.isDict()) {
    throw DL_ABORT_EX(MSG_NULL_TRACKER_RESPONSE);
  }
  const BDE& failure = dict[BtAnnounce::FAILURE_REASON];
  if(failure.isString()) {
    throw DL_ABORT_EX
      (StringFormat(EX_TRACKER_FAILURE, failure.s().c_str()).str());
  }
  const BDE& warn = dict[BtAnnounce::WARNING_MESSAGE];
  if(warn.isString()) {
    logger->warn(MSG_TRACKER_WARNING_MESSAGE, warn.s().c_str());
  }
  const BDE& tid = dict[BtAnnounce::TRACKER_ID];
  if(tid.isString()) {
    trackerId = tid.s();
    if(logger->debug()) {
      logger->debug("Tracker ID:%s", trackerId.c_str());
    }
  }
  const BDE& ival = dict[BtAnnounce::INTERVAL];
  if(ival.isInteger() && ival.i() > 0) {
    interval = ival.i();
    if(logger->debug()) {
      logger->debug("Interval:%d", interval);
    }
  }
  const BDE& mival = dict[BtAnnounce::MIN_INTERVAL];
  if(mival.isInteger() && mival.i() > 0) {
    minInterval = mival.i();
    if(logger->debug()) {
      logger->debug("Min interval:%d", minInterval);
    }
    if(minInterval > interval) {
      minInterval = interval;
    }
  } else {
    // Use interval as a minInterval if minInterval is not supplied.
    minInterval = interval;
  }
  const BDE& comp = dict[BtAnnounce::COMPLETE];
  if(comp.isInteger()) {
    complete = comp.i();
    if(logger->debug()) {
      logger->debug("Complete:%d", complete);
    }
  }
  const BDE& incomp = dict[BtAnnounce::INCOMPLETE];
  if(incomp.isInteger()) {
    incomplete = incomp.i();
    if(logger->debug()) {
      logger->debug("Incomplete:%d", incomplete);
    }
  }
  const BDE& peerData = dict[BtAnnounce::PEERS];
  if(peerData.isNone()) {
    logger->info(MSG_NO_PEER_LIST_RECEIVED);
  } else {
    if(!btRuntime->isHalt() && btRuntime->lessThanMinPeers()) {
      std::vector<SharedHandle<Peer> > peers;
      PeerListProcessor().extractPeer(peerData, std::back_inserter(peers));
      peerStorage->addPeer(peers);
    }
  }
}

bool DefaultBtAnnounce::noMoreAnnounce() {
  return (trackers == 0 &&
          btRuntime->isHalt() &&
          !announceList.countStoppedAllowedTier());
}

void DefaultBtAnnounce::shuffleAnnounce() {
  announceList.shuffle();
}

void DefaultBtAnnounce::setRandomizer(const RandomizerHandle& randomizer)
{
  _randomizer = randomizer;
}

void DefaultBtAnnounce::setBtRuntime(const BtRuntimeHandle& btRuntime)
{
  this->btRuntime = btRuntime;
}

void DefaultBtAnnounce::setPieceStorage(const PieceStorageHandle& pieceStorage)
{
  this->pieceStorage = pieceStorage;
}

void DefaultBtAnnounce::setPeerStorage(const PeerStorageHandle& peerStorage)
{
  this->peerStorage = peerStorage;
}

void DefaultBtAnnounce::overrideMinInterval(time_t interval)
{
  minInterval = interval;
}

} // namespace aria2
