#!/usr/bin/env python3

from flask import Flask, jsonify, request
import os
import pool_keeper_util
import heapq

app = Flask(__name__)

#
# data = [
#     {"id":2, "content":0,  "port": 7002, "hostname":"gpadmin", "datadir": "/home/gpadmin/octopus_internal/gpAux/gpdemo/datadirs/vmpool/dbfast1/"},
#     {"id":3, "content":1,  "port": 7003, "hostname":"gpadmin", "datadir": "/home/gpadmin/octopus_internal/gpAux/gpdemo/datadirs/vmpool/dbfast2/"},
#     {"id":4, "content":2,  "port": 7004, "hostname":"gpadmin", "datadir": "/home/gpadmin/octopus_internal/gpAux/gpdemo/datadirs/vmpool/dbfast3/"},
# ]


def initialization():
    print("🔥 执行初始化操作（应用启动/重载时触发）")
    pool_keeper_util.load_data()

if os.environ.get('WERKZEUG_RUN_MAIN') == 'true' or not app.debug:
    initialization()

# curl http://127.0.0.1:5000/items
# Get all vm
@app.route('/items', methods=['GET'])
def get_items():
    return jsonify(pool_keeper_util.data)

# curl -X POST -H "Content-Type: application/json" -d '{"coordinator_id": 1, "cluster_size":3}' http://127.0.0.1:5000/cluster
# get a cluster
@app.route('/cluster', methods=['POST'])
def get_cluster():
    params = request.json
    if not params or not 'coordinator' in params or not 'executors' in params or not 'load' in params:
        return jsonify({"error": "Invalid data"}), 400

    cluster =  pool_keeper_util.get_workers(params['executors'], params['coordinator'], params['load'])
    return jsonify(cluster), 200

# curl -X POST -H "Content-Type: application/json" -d '{"coordinator_id": 1, "cluster_size":3}' http://127.0.0.1:5000/cluster
# get a cluster
@app.route('/release', methods=['POST'])
def release_cluster():
    params = request.json
    if not params or not 'coordinator' in params or not 'executors' in params:
        return jsonify({"error": "Invalid data"}), 400

    cluster = pool_keeper_util.release_workers(params['executors'], params['coordinator'])
    return jsonify(cluster), 200

# curl http://127.0.0.1:5000/items/1
# Get one vm
@app.route('/item/<int:item_id>', methods=['GET'])
def get_item(item_id):
    for key, value in pool_keeper_util.data.items():
        if key == item_id:
            item = value
            break

    if item:
        return jsonify(item)
    else:
        return jsonify({"error": "Item not found"}), 404

# curl -X POST -H "Content-Type: application/json" -d '{"hostname": "gpadmin", "port":7001, "datadir":"/home/gpadmin/octopus/gpAux/gpdemo/datadirs/cluster1/dbfast1/demoDataDir-1"}' http://127.0.0.1:5000/items
# add new vm
@app.route('/items', methods=['POST'])
def add_item():
    new_item = request.json
    if (not new_item or not 'hostname' in new_item or not 'port' in new_item
        or not 'datadir' in new_item or not 'maxload' in new_item):
        return jsonify({"error": "Invalid data"}), 400

    pool_keeper_util.data[str(len(pool_keeper_util.data) + 2)] = new_item
    new_item["load"] = 0
    pool_keeper_util.write_data()
    return_item = new_item.copy()
    return_item['id'] = len(pool_keeper_util.data) + 1
    return jsonify(return_item  ), 201

# curl -X POST -H "Content-Type: application/json" -d '{"hostname": "gpadmin", "datadir":"/home/gpadmin/octopus_internal/gpAux/gpdemo/datadirs/cluster1/dbfast3/demoDataDir2"}' http://127.0.0.1:5000/itemsdelete
@app.route('/itemsdelete', methods=['POST'])
def delete_item_by_dir():
    new_item = request.json
    if not new_item or not 'hostname' in new_item or not 'datadir' in new_item:
        return jsonify({"error": "Invalid data"}), 400

    for key, item in pool_keeper_util.data.items():
        if item["hostname"] == new_item["hostname"] and item["datadir"] == new_item["datadir"]:
            del pool_keeper_util.data[key]
            pool_keeper_util.write_data()
            return jsonify({"message": "Item deleted"}), 200

    return jsonify({"message": "Item not found"}), 200

# curl http://127.0.0.1:5000/test
@app.route('/test', methods=['GET'])
def test():
    test_data = {
        'id1': {'name':'x', 'load': 5},
        'id2': {'name':'x', 'load': 3},
        'id3': {'name':'x', 'load': 7},
        'id4': {'name':'x', 'load': 1}
    }

    min_items = heapq.nsmallest(2, test_data.items(), key=lambda x: x[1]['load'])
    return jsonify(min_items), 200


if __name__ == '__main__':
    app.run(debug=True)
