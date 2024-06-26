// test that authzGetMore gets called.

if (TestData.testData !== undefined) {
    load(TestData.testData + '/audit/_audit_helpers.js');
} else {
    load('jstests/audit/_audit_helpers.js');
}

var testDBName = 'audit_authz_get_more';

auditTest(
    'authzGetMore',
    function(m) {
        createAdminUserForAudit(m);
        var testDB = m.getDB(testDBName);
        var user = createNoPermissionUserForAudit(m, testDB);

        const beforeCmd = Date.now();

        // Admin should be allowed to perform the operation.
        // NOTE: We expect NOT to see an audit event
        // when an 'admin' user performs this operation.
        var adminDB = m.getDB('admin');
        adminDB.auth('admin','admin');

        // Insert lots of dummy data, if only to help ensure are data set is larger than the
        // returned batch set.
	var n = 1000;
        for (var i = 0; i < n; ++i) {
            assert.writeOK(testDB.foo.insert({'_id': i, s: 'lotsofdummydata'}));
        }

        // Using the admin user, get a bunch of batches, but not all of them.
        var myCursor = testDB.foo.find().batchSize(100);

        // Run getMore as admin with auditAuthorizationSuccess=false
        for (var i = 0; i < 100; i++) {
            printjson(myCursor.next());
        }
        myCursor.next();

        // Run getMore as admin with auditAuthorizationSuccess=true, we should have only
        // one audit event for user admin
        for (i = 1; i < 100; i++) {
            printjson(myCursor.next());
        }
        adminDB.runCommand({ setParameter: 1, 'auditAuthorizationSuccess': true });
        myCursor.next();
        adminDB.runCommand({ setParameter: 1, 'auditAuthorizationSuccess': false });

        // prepare to run getMore as tom
        for (i = 1; i < 100; i++) {
            printjson(myCursor.next());
        }

        adminDB.logout();


        // User (tom) with no permissions logs in.
        var r = testDB.auth('tom', 'tom');
        assert(r);

        // Here, tom tries to use the cursor to get more data.  NOTE: mongo shell calls hasNext()
        // just before calling next().  Since Tom is not authorized for hasNext(), next() (and
        // getMore), the hasNext() call will throw.  We want to ignore that throw, and assert if it
        // does NOT throw.
        assert.throws( function(){ myCursor.next(); } );

        // Tom logs out.
        testDB.logout();

        // Verify that audit event was inserted.
        const beforeLoad = Date.now();
        auditColl = getAuditEventsCollection(m, testDBName, undefined, true);

        // Audit event for user tom
        assert.eq(1, auditColl.count({
            atype: "authCheck",
            ts: withinInterval(beforeCmd, beforeLoad),
            users: { $elemMatch: { user:'tom', db:testDBName} },
            'param.ns': testDBName + '.' + 'foo',
            'param.command': 'getMore',
            result: 13, // <-- Unauthorized error, see error_codes.err...
        }), "FAILED, audit log: " + tojson(auditColl.find().toArray()));

        // Audit event for user admin
        assert.eq(1, auditColl.count({
            atype: "authCheck",
            ts: withinInterval(beforeCmd, beforeLoad),
            users: { $elemMatch: { user:'admin', db:'admin'} },
            'param.ns': testDBName + '.' + 'foo',
            'param.command': 'getMore',
            result: 0, // <-- Authorization successful
        }), "FAILED, audit log: " + tojson(auditColl.find().toArray()));
    },
    { auth:"" }
);
