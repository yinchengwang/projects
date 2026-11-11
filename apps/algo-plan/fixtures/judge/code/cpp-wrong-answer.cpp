#include <iostream>
#include <vector>
using namespace std;

int main() {
    int n;
    if (!(cin >> n)) return 0;
    vector<int> nums(n);
    for (int i = 0; i < n; ++i) cin >> nums[i];
    int target;
    cin >> target;
    // 故意写错：不管输入是什么，一律输出 0 0
    cout << "0 0" << endl;
    return 0;
}
