from flask import Flask, request, jsonify
import os
import base64
import argparse

app = Flask(__name__)

class DiskNode:
    def __init__(self, node_id, storage_path):
        self.node_id = node_id
        self.storage_path = storage_path
        os.makedirs(storage_path, exist_ok=True)

@app.route('/write_block', methods=['POST'])
def write_block():
    data = request.json
    block_id = data.get('block_id')
    block_data = base64.b64decode(data.get('data'))  # Recibimos datos en base64
    is_parity = data.get('is_parity', False)

    # Guardamos el bloque en el filesystem
    filename = f"parity_{block_id}.bin" if is_parity else f"block_{block_id}.bin"
    with open(os.path.join(app.disk_node.storage_path, filename), 'wb') as f:
        f.write(block_data)

    return jsonify({"status": "success", "block_id": block_id})

@app.route('/read_block/<block_id>', methods=['GET'])
def read_block(block_id):
    try:
        # Buscamos tanto bloques de datos como de paridad
        for filename in os.listdir(app.disk_node.storage_path):
            if block_id in filename:
                with open(os.path.join(app.disk_node.storage_path, filename), 'rb') as f:
                    data = base64.b64encode(f.read()).decode('utf-8')
                return jsonify({"status": "success", "data": data})
        return jsonify({"status": "error", "message": "Block not found"}), 404
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/status', methods=['GET'])
def status():
    total_blocks = len(os.listdir(app.disk_node.storage_path))
    return jsonify({
        "node_id": app.disk_node.node_id,
        "storage_path": app.disk_node.storage_path,
        "total_blocks": total_blocks
    })

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, required=True, help='Puerto del servidor')
    parser.add_argument('--node_id', type=int, required=True, help='ID del nodo (1-4)')
    parser.add_argument('--path', type=str, required=True, help='Ruta de almacenamiento')
    args = parser.parse_args()

    app.disk_node = DiskNode(
        node_id=args.node_id,
        storage_path=args.path
    )
    app.run(host='0.0.0.0', port=args.port)