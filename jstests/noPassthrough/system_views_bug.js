(function() {
    'use strict';

    let st = new ShardingTest({shards: 2, mongos: 1});
    let mongos = st.s;
    let db = mongos.getDB('dummy_db');

    let colls1 = db.runCommand({listCollections: 1});
    assert.commandWorked(colls1);

    assert.eq(undefined, colls1.cursor.firstBatch.find(function(c) {
        return c.name === 'system';
    }));

    db.system.views.insertOne({_id: 'dummy_db.a', viewOn: 'b', pipeline: []});

    let colls2 = db.runCommand({listCollections: 1});
    assert.commandWorked(colls2);

    assert.neq(undefined, colls2.cursor.firstBatch.find(function(c) {
        return c.name === 'system';
    }));

    st.stop();
})();
