/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/concurrency/lock_mgr_test_help.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    TEST(Deadlock, NoDeadlock) {
        const ResourceId resId(RESOURCE_DATABASE, std::string("A"));

        LockerForTests locker1(0);
        LockerForTests locker2(1);

        locker1.lockImpl(resId, MODE_S);
        locker2.lockImpl(resId, MODE_S);

        DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
        ASSERT(!wfg1.check().hasCycle());

        DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
        ASSERT(!wfg2.check().hasCycle());
    }

    TEST(Deadlock, Simple) {
        const ResourceId resIdA(RESOURCE_DATABASE, std::string("A"));
        const ResourceId resIdB(RESOURCE_DATABASE, std::string("B"));

        LockerForTests locker1(1);
        LockerForTests locker2(2);

        ASSERT_EQUALS(LOCK_OK, locker1.lockImpl(resIdA, MODE_X));
        ASSERT_EQUALS(LOCK_OK, locker2.lockImpl(resIdB, MODE_X));

        // 1 -> 2
        ASSERT_EQUALS(LOCK_WAITING, locker1.lockImpl(resIdB, MODE_X));

        // 2 -> 1
        ASSERT_EQUALS(LOCK_WAITING, locker2.lockImpl(resIdA, MODE_X));

        DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
        ASSERT(wfg1.check().hasCycle());

        DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
        ASSERT(wfg2.check().hasCycle());

        // Cleanup, so that LockerImpl doesn't complain about leaked locks
        locker1.unlock(resIdB);
        locker2.unlock(resIdA);
    }

    TEST(Deadlock, SimpleUpgrade) {
        const ResourceId resId(RESOURCE_DATABASE, std::string("A"));

        LockerForTests locker1(1);
        LockerForTests locker2(2);

        // Both acquire lock in intent mode
        ASSERT_EQUALS(LOCK_OK, locker1.lockImpl(resId, MODE_IX));
        ASSERT_EQUALS(LOCK_OK, locker2.lockImpl(resId, MODE_IX));

        // Both try to upgrade
        ASSERT_EQUALS(LOCK_WAITING, locker1.lockImpl(resId, MODE_X));
        ASSERT_EQUALS(LOCK_WAITING, locker2.lockImpl(resId, MODE_X));

        DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
        ASSERT(wfg1.check().hasCycle());

        DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
        ASSERT(wfg2.check().hasCycle());

        // Cleanup, so that LockerImpl doesn't complain about leaked locks
        locker1.unlock(resId);
        locker2.unlock(resId);
    }

    TEST(Deadlock, Indirect) {
        const ResourceId resIdA(RESOURCE_DATABASE, std::string("A"));
        const ResourceId resIdB(RESOURCE_DATABASE, std::string("B"));

        LockerForTests locker1(1);
        LockerForTests locker2(2);
        LockerForTests lockerIndirect(3);

        ASSERT_EQUALS(LOCK_OK, locker1.lockImpl(resIdA, MODE_X));
        ASSERT_EQUALS(LOCK_OK, locker2.lockImpl(resIdB, MODE_X));

        // 1 -> 2
        ASSERT_EQUALS(LOCK_WAITING, locker1.lockImpl(resIdB, MODE_X));

        // 2 -> 1
        ASSERT_EQUALS(LOCK_WAITING, locker2.lockImpl(resIdA, MODE_X));

        // 3 -> 2
        ASSERT_EQUALS(LOCK_WAITING, lockerIndirect.lockImpl(resIdA, MODE_X));

        DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
        ASSERT(wfg1.check().hasCycle());

        DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
        ASSERT(wfg2.check().hasCycle());

        // Indirect locker should not report the cycle since it does not participate in it
        DeadlockDetector wfgIndirect(*getGlobalLockManager(), &lockerIndirect);
        ASSERT(!wfgIndirect.check().hasCycle());

        // Cleanup, so that LockerImpl doesn't complain about leaked locks
        locker1.unlock(resIdB);
        locker2.unlock(resIdA);
    }

    TEST(Deadlock, IndirectWithUpgrade) {
        const ResourceId resIdFlush(RESOURCE_MMAPV1_FLUSH, 1);
        const ResourceId resIdDb(RESOURCE_DATABASE, 2);

        LockerForTests flush(1);
        LockerForTests reader(2);
        LockerForTests writer(3);

        // This sequence simulates the deadlock which occurs during flush
        ASSERT_EQUALS(LOCK_OK, writer.lockImpl(resIdFlush, MODE_IX));
        ASSERT_EQUALS(LOCK_OK, writer.lockImpl(resIdDb, MODE_X));

        ASSERT_EQUALS(LOCK_OK, reader.lockImpl(resIdFlush, MODE_IS));

        // R -> W
        ASSERT_EQUALS(LOCK_WAITING, reader.lockImpl(resIdDb, MODE_S));

        // R -> W
        // F -> W
        ASSERT_EQUALS(LOCK_WAITING, flush.lockImpl(resIdFlush, MODE_S));

        // W yields its flush lock, so now f is granted in mode S
        //
        // R -> W
        writer.unlock(resIdFlush);

        // Flush thread upgrades S -> X in order to do the remap
        //
        // R -> W
        // F -> R
        ASSERT_EQUALS(LOCK_WAITING, flush.lockImpl(resIdFlush, MODE_X));

        // W comes back from the commit and tries to re-acquire the flush lock
        //
        // R -> W
        // F -> R
        // W -> F
        ASSERT_EQUALS(LOCK_WAITING, writer.lockImpl(resIdFlush, MODE_IX));

        // Run deadlock detection from the point of view of each of the involved lockers
        DeadlockDetector wfgF(*getGlobalLockManager(), &flush);
        ASSERT(wfgF.check().hasCycle());

        DeadlockDetector wfgR(*getGlobalLockManager(), &reader);
        ASSERT(wfgR.check().hasCycle());

        DeadlockDetector wfgW(*getGlobalLockManager(), &writer);
        ASSERT(wfgW.check().hasCycle());

        // Cleanup, so that LockerImpl doesn't complain about leaked locks
        flush.unlock(resIdFlush);
        writer.unlock(resIdFlush);
    }

} // namespace mongo
