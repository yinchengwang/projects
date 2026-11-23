from core.fusion import rrf_fuse, fuse_hits


def test_rrf_order_and_configurable_k():
    result = rrf_fuse(["a", "b"], ["b", "a"], k=60)
    assert result[0][0] == "a"
    assert result[1][0] == "b"
    assert result[0][1] == 1 / 61 + 1 / 62


def test_overview_is_demoted():
    hits = fuse_hits([("overview", 1.0), ("feature", 0.9)],
                     [("overview", 3.0), ("feature", 2.0)],
                     documents={"overview": {"is_overview": True}, "feature": {"is_overview": False}})
    assert hits[0]["block_id"] == "feature"
